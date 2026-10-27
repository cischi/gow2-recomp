// Common GPU module (runtime/gpu/gpu_system.*): backend selection and
// FIFO interrupts. Test backend without a command processor; the "command
// processor" is a guest thread of the test that sends interrupts in a loop.
// Real GuestMemory arena, no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "gpu/gpu_system.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/sync.h"
#include "kernel/threads.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Instant of the last OnVblank: the vblank takes the interrupts' turn right away.
std::atomic<int64_t> g_vblank_marked_ns{0};
std::atomic<int> g_on_vblank{0}, g_starts{0}, g_stops{0};

class TestBackend final : public gpu::Backend {
 public:
  bool Start() override {
    ++g_starts;
    return true;
  }
  void Stop() override { ++g_stops; }
  void InitializeRingBuffer(uint32_t, uint32_t) override {}
  void EnableReadPointerWriteBack(uint32_t, uint32_t) override {}
  void OnVblank() override {
    g_vblank_marked_ns = NowNs();
    ++g_on_vblank;
  }
  gpu::Counters GetCounters() const override { return {1, 2, 3}; }
};

// Callback: vblank (source 0) measures the wait for the turn; source 1 occupies 1 ms.
constexpr uint32_t kFnInterrupt = 0x82000100;
std::atomic<int> g_vblanks{0}, g_cp_calls{0};
std::atomic<int64_t> g_max_vblank_wait_ns{0};

PPC_FUNC(FnInterrupt) {
  if (ctx.r3.u32 == 0) {
    const int64_t wait = NowNs() - g_vblank_marked_ns.load();
    int64_t prev = g_max_vblank_wait_ns.load();
    while (wait > prev && !g_max_vblank_wait_ns.compare_exchange_weak(prev, wait)) {
    }
    ++g_vblanks;
  } else {
    const auto end = Clock::now() + 1ms;  // busy wait: Sleep would round up
    while (Clock::now() < end) {
    }
    ++g_cp_calls;
  }
}

PPCFunc* Resolve(uint32_t guest) { return guest == kFnInterrupt ? &FnInterrupt : nullptr; }

struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    kernel::g_base = mem.base;
    REQUIRE(kernel::InitMemory(mem));
    REQUIRE(kernel::InitThreads(mem, &Resolve, 0x40000));
    REQUIRE(gpu::Init(mem, std::make_unique<TestBackend>()));
  }
  ~Env() { gpu::Shutdown(); }
};

Env& E() {
  static Env env;
  return env;
}

}  // namespace

TEST_CASE("gpu_system: ParseBackendKind accepts only lowercase fake and d3d12", "[gpu]") {
  CHECK(gpu::ParseBackendKind("fake") == gpu::BackendKind::kFake);
  CHECK(gpu::ParseBackendKind("d3d12") == gpu::BackendKind::kD3D12);
  CHECK_FALSE(gpu::ParseBackendKind("D3D12").has_value());
  CHECK_FALSE(gpu::ParseBackendKind("Fake").has_value());
  CHECK_FALSE(gpu::ParseBackendKind("").has_value());
  CHECK_FALSE(gpu::ParseBackendKind("vulkan").has_value());
}

TEST_CASE("gpu_system: Init starts the backend, the vblank calls OnVblank, Shutdown stops it", "[gpu]") {
  E();
  CHECK(g_starts == 1);
  CHECK(gpu::ReadRegister(0x0F00) == 0x08100748);  // RB_EDRAM_TIMING
  CHECK(gpu::GetCounters().unknown_packets == 2);
  const int before = g_on_vblank.load();
  std::this_thread::sleep_for(100ms);
  CHECK(g_on_vblank.load() > before);
  gpu::Shutdown();
  CHECK(g_stops == 1);
  CHECK(gpu::GetCounters().frames == 3);  // last counters of the backend
  const int after = g_on_vblank.load();
  std::this_thread::sleep_for(50ms);
  CHECK(g_on_vblank.load() == after);
  gpu::Shutdown();  // idempotent
  CHECK(g_stops == 1);
}

// A command processor that sends interrupts in a loop (1 ms callbacks) does not
// starve the vblank: turns are FIFO, the vblank waits at most
// for the interrupt in progress.
TEST_CASE("gpu_system: FIFO interrupts, the vblank does not wait behind a CP interrupt loop", "[gpu]") {
  E();
  gpu::SetInterruptCallback(kFnInterrupt, 0x1234);
  std::atomic<bool> stop{false};
  auto cp = kernel::GuestThread::CreateHost(
      [&stop] {
        while (!stop.load()) gpu::DispatchInterrupt(1, 0);
        return 0u;
      },
      0, "test CP");
  REQUIRE(cp);
  // measured after the CP has started
  REQUIRE([] {
    const auto end = Clock::now() + 1s;
    while (g_cp_calls.load() == 0 && Clock::now() < end) std::this_thread::sleep_for(1ms);
    return g_cp_calls.load() > 0;
  }());
  g_vblanks = 0;
  g_max_vblank_wait_ns = 0;
  const int cp_before = g_cp_calls.load();
  std::this_thread::sleep_for(1s);
  const int vblanks = g_vblanks.load();
  const double max_wait_ms = g_max_vblank_wait_ns.load() / 1e6;
  const int cp_calls = g_cp_calls.load() - cp_before;
  stop = true;
  const std::shared_ptr<kernel::KernelObject> one[1] = {cp};
  kernel::WaitAny(one, nullptr, false);
  cp.reset();
  gpu::SetInterruptCallback(0, 0);
  INFO("vblanks served " << vblanks << ", maximum wait " << max_wait_ms << " ms, CP interrupts " << cp_calls);
  CHECK(cp_calls >= 300);  // the CP really occupied the lock
  CHECK(vblanks >= 50);
  CHECK(max_wait_ms <= 3.0);
}

// The emulated vblank period is configurable hot (frame rate 120/144).
TEST_CASE("gpu_system: SetVblankPeriod changes the OnVblank frequency", "[gpu]") {
  E();
  gpu::SetVblankPeriod(std::chrono::microseconds(8333));  // 120 Hz
  std::this_thread::sleep_for(100ms);                     // the vblank rereads the period on every iteration
  const int before = g_on_vblank.load();
  std::this_thread::sleep_for(500ms);
  const int n = g_on_vblank.load() - before;
  gpu::SetVblankPeriod(gpu::kVblankPeriod);
  INFO("OnVblank in 500 ms: " << n);
  CHECK(n >= 50);
  CHECK(n <= 65);
}
