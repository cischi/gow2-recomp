// XMA decoder (xma_decoder.h). Xenia reference @ 95a5c3ee (BSD):
// src/xenia/apu/xma_decoder.cc (WorkerThreadMain, WriteRegister,
// AllocateContext/ReleaseContext).
#include "apu/xma_decoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "guest_memory.h"
#include "kernel/xma.h"
#include "log.h"
#include "platform/platform.h"
#include "xenia/apu/xma_context_new.h"
#include "xenia/apu/xma_context_old.h"
#include "xenia/memory.h"

namespace apu {
namespace {

using kernel::xma::kContextCount;
using kernel::xma::kContextSize;

// Xenia (WorkerThreadMain): after 500 loops without work, wait for the event
// at most 20 ms per loop.
constexpr uint32_t kIdleLoopsBeforeWait = 500;
constexpr auto kIdleWait = std::chrono::milliseconds(20);

void Fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }

struct Slot {
  // XmaContextNew or XmaContextOld from xenia-canary; created once, on the first Start.
  std::unique_ptr<xe::apu::XmaContext> context;
  // Held by the worker for the whole Work and by Kick/Lock/Clear, allocation
  // and release (xma_decoder.h, lock order).
  std::mutex mutex;
  // Copies readable without a lock (worker, GetXmaContextState); written only
  // under `mutex`.
  std::atomic<bool> allocated{false};
  std::atomic<bool> enabled{false};
};

// Created on the first Start and never destroyed: a listener running on a
// guest thread may still use them after Stop.
struct Decoder {
  std::unique_ptr<xe::Memory> memory;
  std::unique_ptr<Slot[]> slots;
};
std::atomic<Decoder*> g_decoder{nullptr};

// Worker state, protected by g_control (Start/Stop).
std::mutex g_control;
std::thread g_worker;
XmaWorkFn g_work;

// Work event (auto-reset, like Xenia's work_event_) and exit flag.
std::mutex g_event_mutex;
std::condition_variable g_event_cv;
bool g_event_set = false;
std::atomic<bool> g_running{false};

void SignalWork() {
  {
    std::lock_guard lock(g_event_mutex);
    g_event_set = true;
  }
  g_event_cv.notify_one();
}

void WaitWork() {
  std::unique_lock lock(g_event_mutex);
  g_event_cv.wait_for(lock, kIdleWait, [] { return g_event_set || !g_running.load(); });
  g_event_set = false;
}

Slot& SlotAt(uint32_t index) { return g_decoder.load(std::memory_order_acquire)->slots[index]; }

// Real Work: XmaContext::Work (Decode on the context's 64 bytes and on the
// game's physical buffers).
bool DefaultWork(uint32_t index) {
  Slot& slot = SlotAt(index);
  Fence();
  const bool worked = slot.context->Work();
  Fence();
  return worked;
}

// Xenia: XmaDecoder::WorkerThreadMain.
void WorkerMain(XmaWorkFn work) {
  plat::SetCurrentThreadName("XMA Decoder");
  uint32_t idle_loops = 0;
  while (g_running.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (uint32_t i = 0; i < kContextCount; ++i) {
      Slot& slot = SlotAt(i);
      if (!slot.enabled.load(std::memory_order_acquire)) continue;
      std::lock_guard lock(slot.mutex);
      if (!slot.allocated.load(std::memory_order_relaxed) || !slot.enabled.load(std::memory_order_relaxed))
        continue;
      did_work = work(i) || did_work;
      // The real Work clears `is_enabled` (one Kick = one Decode pass).
      slot.enabled.store(slot.context->is_enabled(), std::memory_order_release);
    }
    idle_loops = did_work ? 0 : idle_loops + 1;
    if (idle_loops > kIdleLoopsBeforeWait) {
      WaitWork();
    } else {
      std::this_thread::yield();
    }
  }
}

// Xenia: XmaDecoder::WriteRegister (Kick/Lock/Clear). On the game thread
// that writes the register.
void OnRegisterOp(kernel::xma::RegisterOp op, uint32_t first_context, uint32_t bits) {
  Decoder* decoder = g_decoder.load(std::memory_order_acquire);
  if (!decoder) return;
  bool kicked = false;
  for (uint32_t b = 0; bits && b < 32; ++b, bits >>= 1) {
    if (!(bits & 1)) continue;
    const uint32_t index = first_context + b;
    if (index >= kContextCount) break;
    Slot& slot = decoder->slots[index];
    std::lock_guard lock(slot.mutex);  // waits for the running Work
    if (!slot.allocated.load(std::memory_order_relaxed)) continue;
    switch (op) {
      case kernel::xma::RegisterOp::kKick:
        Fence();
        slot.context->Enable();
        Fence();
        slot.enabled.store(true, std::memory_order_release);
        kicked = true;
        break;
      case kernel::xma::RegisterOp::kLock:
        slot.context->Disable();
        slot.enabled.store(false, std::memory_order_release);
        break;
      case kernel::xma::RegisterOp::kClear:
        Fence();
        slot.context->Clear();
        Fence();
        break;
    }
  }
  if (kicked) SignalWork();
}

// Xenia: XmaDecoder::AllocateContext / ReleaseContext (XmaContext::Release).
// Under the kernel::xma mutex; the kernel zeroes the 64 bytes right after the
// release.
void OnContext(uint32_t index, bool allocated) {
  Decoder* decoder = g_decoder.load(std::memory_order_acquire);
  if (!decoder || index >= kContextCount) return;
  Slot& slot = decoder->slots[index];
  std::lock_guard lock(slot.mutex);  // waits for the running Work
  // With the slot mutex held no Work is running: the flag is enough (Disable
  // would write an [xenia] A line for every context).
  slot.context->set_is_enabled(false);
  slot.enabled.store(false, std::memory_order_release);
  slot.context->set_is_allocated(allocated);
  slot.allocated.store(allocated, std::memory_order_release);
}

}  // namespace

std::optional<XmaDecoderKind> ParseXmaDecoder(std::string_view name) {
  if (name == "new") return XmaDecoderKind::kNew;
  if (name == "old") return XmaDecoderKind::kOld;
  return std::nullopt;
}

const char* XmaDecoderName(XmaDecoderKind kind) { return kind == XmaDecoderKind::kOld ? "old" : "new"; }

bool StartXmaDecoder(GuestMemory& mem, XmaWorkFn work, XmaDecoderKind kind) {
  std::lock_guard control(g_control);
  if (g_running.load()) return true;
  const uint32_t array = kernel::xma::ContextArray();
  if (!array) {
    LogLine("[apu] XMA decoder: XMA registers not initialized");
    return false;
  }
  Decoder* existing = g_decoder.load();
  if (!existing) {
    auto decoder = std::make_unique<Decoder>();
    decoder->memory = std::make_unique<xe::Memory>(mem.base);
    decoder->slots = std::make_unique<Slot[]>(kContextCount);
    for (uint32_t i = 0; i < kContextCount; ++i) {
      if (kind == XmaDecoderKind::kOld) decoder->slots[i].context = std::make_unique<xe::apu::XmaContextOld>();
      else decoder->slots[i].context = std::make_unique<xe::apu::XmaContextNew>();
      if (decoder->slots[i].context->Setup(i, decoder->memory.get(), array + i * kContextSize) != 0) {
        LogLine("[apu] XMA decoder: cannot create context " + std::to_string(i));
        return false;
      }
    }
    existing = decoder.release();
    g_decoder.store(existing, std::memory_order_release);
  } else if (existing->memory->virtual_membase() != mem.base ||
             existing->slots[0].context->guest_ptr() != array) {
    LogLine("[apu] XMA decoder: arena or context array differ from the first Start");
    return false;
  }
  // All free and disabled (the listeners are not installed yet).
  for (uint32_t i = 0; i < kContextCount; ++i) {
    Slot& slot = existing->slots[i];
    std::lock_guard lock(slot.mutex);
    slot.context->set_is_enabled(false);
    slot.context->set_is_allocated(false);
    slot.enabled.store(false);
    slot.allocated.store(false);
  }
  g_work = work ? std::move(work) : XmaWorkFn(&DefaultWork);
  {
    std::lock_guard lock(g_event_mutex);
    g_event_set = false;
  }
  g_running.store(true, std::memory_order_release);
  g_worker = std::thread(&WorkerMain, g_work);
  kernel::xma::SetContextListener(&OnContext);
  kernel::xma::SetRegisterListener(&OnRegisterOp);
  return true;
}

void StopXmaDecoder() {
  std::lock_guard control(g_control);
  kernel::xma::SetRegisterListener(nullptr);
  kernel::xma::SetContextListener(nullptr);
  if (!g_running.exchange(false)) return;
  SignalWork();
  if (g_worker.joinable()) g_worker.join();
  g_work = nullptr;
}

XmaContextState GetXmaContextState(uint32_t index) {
  Decoder* decoder = g_decoder.load(std::memory_order_acquire);
  if (!decoder || index >= kContextCount) return {false, false};
  const Slot& slot = decoder->slots[index];
  return {slot.allocated.load(std::memory_order_acquire), slot.enabled.load(std::memory_order_acquire)};
}

}  // namespace apu
