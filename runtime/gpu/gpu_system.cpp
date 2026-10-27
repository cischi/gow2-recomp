// Common GPU module: registers, vblank, interrupts, forwarding to the backend (see
// gpu_system.h).
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/gpu/graphics_system.cc (Setup: thread "GPU VSync";
// ReadRegister/WriteRegister; SetInterruptCallback;
// DispatchInterruptCallback; MarkVblank) e src/xenia/cpu/processor.cc
// (ExecuteInterrupt).
#include "gpu/gpu_system.h"

// First: kernel/function.h (GuestToHostFunction) before the headers that
// would bring in ppc_config.h (see kernel/function.h).
#include "kernel/function.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "gpu/xenos.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"
#include "platform/platform.h"

namespace gpu {
namespace {

// Registers that Xenia (GraphicsSystem::ReadRegister) always reads with these values.
struct FixedRegister {
  uint32_t reg, value;
};
constexpr FixedRegister kFixedRegisters[] = {
    {0x0F00, 0x08100748},  // RB_EDRAM_TIMING
    {0x0F01, 0x0000200E},  // RB_BC_CONTROL
    {0x194C, 0x000002D0},  // R500_D1MODE_V_COUNTER
    {0x1951, 0x00000001},  // interrupt status: vblank
    {0x1961, 0x050002D0},  // AVIVO_D1MODE_VIEWPORT_SIZE: 1280x720
};

// Ticket lock: interrupts are served in arrival order. The operating
// system mutex is not fair: a command processor that sends interrupts
// in a loop could always take it again before the vblank.
class TicketLock {
 public:
  void lock() {
    const uint32_t ticket = next_.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t serving = serving_.load(std::memory_order_acquire); serving != ticket;
         serving = serving_.load(std::memory_order_acquire))
      serving_.wait(serving, std::memory_order_acquire);
  }
  void unlock() {
    serving_.fetch_add(1, std::memory_order_release);
    serving_.notify_all();
  }

 private:
  std::atomic<uint32_t> next_{0};
  std::atomic<uint32_t> serving_{0};
};

// Global state never destroyed (guest threads may survive until exit).
struct State {
  std::mutex mutex;  // Init/Shutdown, backend
  bool initialized = false;
  std::atomic<bool> running{false};  // vblank thread
  std::unique_ptr<Backend> backend;
  Counters last_counters;  // after Shutdown

  std::atomic<uint64_t> callback{0};  // (callback << 32) | user_data
  TicketLock interrupt_lock;

  std::shared_ptr<kernel::GuestThread> vblank_thread;
};

State& S() {
  static State* s = new State;
  return *s;
}

uint32_t* RegPtr(uint32_t reg) { return kernel::GuestPtr<uint32_t>(kRegisterBase + reg * 4); }

void PresetFixedRegisters() {
  for (const auto& f : kFixedRegisters) WriteRegister(f.reg, f.value);
}

// Xenia GraphicsSystem "GPU VSync" + MarkVblank: every 16.667 ms counter + 1
// and callback(0, user_data) on CPU 2.
std::atomic<int64_t> g_vblank_period_us{kVblankPeriod.count()};

uint32_t VblankMain(Backend* backend) {
  State& s = S();
  auto next = std::chrono::steady_clock::now() + std::chrono::microseconds(g_vblank_period_us.load());
  while (s.running.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next) {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(next - now).count();
      plat::SleepPrecise(uint32_t(std::min<int64_t>(us, 2000)));  // in short steps: Shutdown does not wait
      continue;
    }
    // Late by more than a period (slow callback): restart from now.
    const std::chrono::microseconds period(g_vblank_period_us.load());
    next = (now - next > period) ? now + period : next + period;
    backend->OnVblank();
    PresetFixedRegisters();
    DispatchInterrupt(0, 2);
  }
  return 0;
}

void WaitAndRelease(std::shared_ptr<kernel::GuestThread>& t) {
  if (!t) return;
  const std::shared_ptr<kernel::KernelObject> one[1] = {t};
  kernel::WaitAny(one, nullptr, false);
  t.reset();  // the thread has finished: the destructor joins
}

}  // namespace

std::optional<BackendKind> ParseBackendKind(std::string_view name) {
  if (name == "fake") return BackendKind::kFake;
  if (name == "d3d12") return BackendKind::kD3D12;
  return std::nullopt;
}

bool Init(GuestMemory& mem, std::unique_ptr<Backend> backend) {
  State& s = S();
  std::unique_lock g(s.mutex);
  if (s.initialized) return true;
  if (!backend) return false;
  if (!mem.Commit(kRegisterBase, kRegisterRegionSize)) {
    LogLine("[gpu] cannot commit the GPU registers");
    return false;
  }
  std::memset(mem.base + kRegisterBase, 0, kRegisterRegionSize);
  PresetFixedRegisters();
  s.callback = 0;
  s.last_counters = {};
  if (!backend->Start()) return false;  // the backend logs the reason
  Backend* raw = backend.get();
  s.running = true;
  s.vblank_thread = kernel::GuestThread::CreateHost([raw] { return VblankMain(raw); }, 0, "GPU VSync");
  if (!s.vblank_thread) {
    LogLine("[gpu] cannot create the vblank thread");
    s.running = false;
    g.unlock();  // the command processor may call the Vd* (s.mutex)
    backend->Stop();
    return false;
  }
  s.backend = std::move(backend);
  s.initialized = true;
  return true;
}

void Shutdown() {
  State& s = S();
  std::shared_ptr<kernel::GuestThread> vblank;
  std::unique_ptr<Backend> backend;
  {
    std::lock_guard g(s.mutex);
    if (!s.initialized) return;
    s.initialized = false;
    s.running = false;
    vblank = std::move(s.vblank_thread);
    backend = std::move(s.backend);
  }
  // Outside the lock: the interrupt callbacks may call the Vd*.
  WaitAndRelease(vblank);  // uses the backend: before destroying it
  backend->Stop();
  const Counters last = backend->GetCounters();
  backend.reset();
  std::lock_guard g(s.mutex);
  s.last_counters = last;
}

uint32_t ReadRegister(uint32_t reg) {
  if (reg >= kRegisterCount) return 0;
  return std::byteswap(std::atomic_ref<uint32_t>(*RegPtr(reg)).load(std::memory_order_acquire));
}

void WriteRegister(uint32_t reg, uint32_t value) {
  if (reg < kRegisterCount)
    std::atomic_ref<uint32_t>(*RegPtr(reg)).store(std::byteswap(value), std::memory_order_release);
}

void InitializeRingBuffer(uint32_t phys, uint32_t size_log2) {
  State& s = S();
  std::lock_guard g(s.mutex);
  if (s.backend) s.backend->InitializeRingBuffer(phys, size_log2);
}

void EnableReadPointerWriteBack(uint32_t phys, uint32_t block_size_log2) {
  State& s = S();
  std::lock_guard g(s.mutex);
  if (s.backend) s.backend->EnableReadPointerWriteBack(phys, block_size_log2);
}

// Xenia GraphicsSystem::SetInterruptCallback.
void SetVblankPeriod(std::chrono::microseconds period) { g_vblank_period_us = period.count(); }

void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  S().callback = callback ? (uint64_t(callback) << 32) | user_data : 0;
}

// Xenia GraphicsSystem::DispatchInterruptCallback and, like Processor::
// ExecuteInterrupt (src/xenia/cpu/processor.cc): one interrupt at a time
// (Xenia holds the global critical region: vblank and command
// processor interrupts never overlap) and the PCR TLS pointer (PCR+0) set to 0
// during the callback, restored afterwards ("some games check this and
// early-exit routines when under interrupts"). Deviations: the lock is only
// among the GPU interrupts (the kernel has no global critical region
// that guest code can take) and it is FIFO.
void DispatchInterrupt(uint32_t source, uint32_t cpu) {
  State& s = S();
  if (!s.callback.load()) return;
  std::lock_guard lock(s.interrupt_lock);
  // Reread with the lock: a callback removed while waiting for its turn is not called.
  const uint64_t cb = s.callback.load();
  const uint32_t callback = uint32_t(cb >> 32), user_data = uint32_t(cb);
  if (!callback) return;
  kernel::GuestThread* t = kernel::GuestThread::Current();
  if (t) t->SetAffinity(1u << cpu);
  be<uint32_t>* tls_ptr = t ? kernel::GuestPtr<be<uint32_t>>(t->PcrAddr() + kernel::kPcrTlsPtr) : nullptr;
  const uint32_t old_tls = tls_ptr ? uint32_t(*tls_ptr) : 0;
  if (tls_ptr) *tls_ptr = 0;
  GuestToHostFunction<void>(callback, source, user_data);
  if (tls_ptr) *tls_ptr = old_tls;
}

Counters GetCounters() {
  State& s = S();
  std::lock_guard g(s.mutex);
  return s.backend ? s.backend->GetCounters() : s.last_counters;
}

bool LogDeviceRemovedOnce(uint32_t reason) {
  static std::atomic_flag logged;
  if (logged.test_and_set()) return false;
  char line[64];
  std::snprintf(line, sizeof(line), "[gpu] device removed reason=0x%08X", unsigned(reason));
  LogLine(line);
  return true;
}

}  // namespace gpu
