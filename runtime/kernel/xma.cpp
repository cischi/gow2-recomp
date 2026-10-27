// "Deviceless" XMA decoder (xma.h). Xenia references @ 95a5c3ee (BSD):
// src/xenia/apu/xma_decoder.cc, src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc.
#include "kernel/xma.h"

#include <atomic>
#include <bit>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "kernel/stats.h"
#include "log.h"

namespace kernel::xma {
namespace {

constexpr uint32_t kRegisterCount = kRegisterRegionSize / 4;
constexpr uint32_t kStatusNoMemory = 0xC0000017;

std::mutex g_mutex;
// Written under g_mutex (Init/Shutdown), read without a lock by Read/WriteRegister
// on the game and hook threads.
std::atomic<bool> g_initialized{false};
MemorySystem* g_memory = nullptr;  // heap the array comes from
uint32_t g_array = 0;              // guest address of context 0
std::bitset<kContextCount> g_used;
std::atomic<RegisterListener> g_listener{nullptr};
std::atomic<ContextListener> g_context_listener{nullptr};

// Groups of 10 registers (32 contexts each): Kick, Lock, Clear.
constexpr uint32_t kRegistersPerOp = kContextCount / 32;

// Register as a host DWORD (little-endian, as lwbrx reads it); the region is
// 64 KiB-aligned, so every register is 4-aligned.
std::atomic_ref<uint32_t> Reg(uint32_t reg) {
  return std::atomic_ref<uint32_t>(*GuestPtr<uint32_t>(kRegisterBase + reg * 4));
}

}  // namespace

uint32_t ReadRegister(uint32_t reg) {
  if (reg >= kRegisterCount || !g_initialized.load(std::memory_order_acquire)) return 0;
  return Reg(reg).load(std::memory_order_relaxed);  // little-endian (host)
}

void WriteRegister(uint32_t reg, uint32_t value) {
  if (reg >= kRegisterCount || !g_initialized.load(std::memory_order_acquire)) return;
  Reg(reg).store(value, std::memory_order_relaxed);
}

void SetRegisterListener(RegisterListener listener) { g_listener.store(listener, std::memory_order_release); }

void SetContextListener(ContextListener listener) {
  g_context_listener.store(listener, std::memory_order_release);
}

// Xenia: XmaDecoder::WriteRegister (register in the file, then Kick/Lock/Clear
// for the bits set to 1, context = 32 * (reg - base) + bit).
void OnRegisterWrite(uint32_t reg, uint32_t value) {
  if (reg >= kRegisterCount) return;
  WriteRegister(reg, value);
  RegisterOp op;
  uint32_t group;
  if (reg >= kRegContext0Kick && reg < kRegContext0Kick + kRegistersPerOp) {
    op = RegisterOp::kKick;
    group = reg - kRegContext0Kick;
    stats::NoteXmaKicks(static_cast<uint32_t>(std::popcount(value)));
  } else if (reg >= kRegContext0Lock && reg < kRegContext0Lock + kRegistersPerOp) {
    op = RegisterOp::kLock;
    group = reg - kRegContext0Lock;
  } else if (reg >= kRegContext0Clear && reg < kRegContext0Clear + kRegistersPerOp) {
    op = RegisterOp::kClear;
    group = reg - kRegContext0Clear;
  } else {
    return;
  }
  if (value == 0) return;
  if (RegisterListener listener = g_listener.load(std::memory_order_acquire)) listener(op, group * 32, value);
}

// Xenia: XmaDecoder::Setup (registers to 0, context array in the physical
// heap, ContextArrayAddress = physical address, NextContextIndex = 1).
bool Init(GuestMemory& mem) {
  std::lock_guard lock(g_mutex);
  if (g_initialized) return true;
  MemorySystem* m = Memory();
  if (!m) return false;
  if (!mem.Commit(kRegisterBase, kRegisterRegionSize)) return false;
  std::memset(mem.base + kRegisterBase, 0, kRegisterRegionSize);
  const uint32_t array = m->phys.Alloc(kContextCount * kContextSize, 0x1000);
  if (!array) return false;
  std::memset(GuestPtr<uint8_t>(array), 0, kContextCount * kContextSize);
  g_memory = m;
  g_array = array;
  g_used.reset();
  g_initialized.store(true, std::memory_order_release);
  WriteRegister(kRegContextArrayAddress, array & 0x1FFFFFFF);  // MmGetPhysicalAddress
  WriteRegister(kRegNextContextIndex, 1);
  return true;
}

void Shutdown() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized) return;
  if (g_memory && g_memory == Memory()) g_memory->phys.Free(g_array);
  g_initialized.store(false, std::memory_order_release);
  g_memory = nullptr;
  g_array = 0;
  g_used.reset();
}

uint32_t ContextArray() {
  std::lock_guard lock(g_mutex);
  return g_array;
}

// Xenia: XmaDecoder::AllocateContext (first free bitmap index).
uint32_t AllocateContext() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized) return 0;
  for (uint32_t i = 0; i < kContextCount; ++i) {
    if (g_used[i]) continue;
    g_used[i] = true;
    const uint32_t ctx = g_array + i * kContextSize;
    std::memset(GuestPtr<uint8_t>(ctx), 0, kContextSize);
    if (ContextListener listener = g_context_listener.load(std::memory_order_acquire)) listener(i, true);
    return ctx;
  }
  return 0;
}

// Xenia: XmaDecoder::ReleaseContext -> XmaContext::Release (zeroes the 64 bytes).
bool ReleaseContext(uint32_t ctx) {
  std::lock_guard lock(g_mutex);
  if (!g_initialized || ctx < g_array || ctx >= g_array + kContextCount * kContextSize) return false;
  const uint32_t offset = ctx - g_array;
  if (offset % kContextSize) return false;
  const uint32_t i = offset / kContextSize;
  if (!g_used[i]) return false;
  g_used[i] = false;
  // Before zeroing: the decoder disables the context and waits for the
  // Work in progress (Xenia: XmaContext::Release under the context lock).
  if (ContextListener listener = g_context_listener.load(std::memory_order_acquire)) listener(i, false);
  std::memset(GuestPtr<uint8_t>(ctx), 0, kContextSize);
  return true;
}

namespace {

// Xenia: XMACreateContext_entry: context from the decoder's array, 0 and
// X_STATUS_NO_MEMORY if exhausted.
uint32_t XMACreateContextImpl(be<uint32_t>* out) {
  const uint32_t ctx = AllocateContext();
  if (out) *out = ctx;
  return ctx ? 0 : kStatusNoMemory;
}
// Xenia: XMAReleaseContext_entry.
uint32_t XMAReleaseContextImpl(uint32_t ctx) {
  ReleaseContext(ctx);
  return 0;
}

}  // namespace
}  // namespace kernel::xma

// Called by PPC_MM_STORE_U32 (runtime/ppc_detail.h) after the normal store.
// An address not 4-aligned is not a register: the store stays, the hook
// does not. With --trace the first 32 writes go to the log
// (`[xma] reg 0x%03X = 0x%08X`): run-time proof that the hook fires in the game.
extern "C" void gow2_mmio_store32(uint32_t guest_address, uint32_t value) {
  if (guest_address & 3) return;
  const uint32_t reg = (guest_address - kernel::xma::kRegisterBase) / 4;
  if (kernel::stats::Trace()) {
    static std::atomic<uint32_t> traced{0};
    if (traced.fetch_add(1, std::memory_order_relaxed) < 32) {
      char line[48];
      std::snprintf(line, sizeof(line), "[xma] reg 0x%03X = 0x%08X", reg, value);
      LogLine(line);
    }
  }
  kernel::xma::OnRegisterWrite(reg, value);
}

KERNEL_EXPORT(XMACreateContext, kernel::xma::XMACreateContextImpl)
KERNEL_EXPORT(XMAReleaseContext, kernel::xma::XMAReleaseContextImpl)
