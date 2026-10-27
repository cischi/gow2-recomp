// Test bench of the --gpu d3d12 backend on synthetic rings: real GuestMemory
// arena with the kernel and physwatch initialized (like the game), Xenia cvars
// set before the backend, gpu::CreateD3D12Backend without a window (no
// presenter), gpu::Init, 32 KiB ring with read pointer write-back,
// warm-up with a NOP (SetupContext with a cold driver cache exceeds 2 s).
// Run() copies a Pm4Builder into the ring with a MEM_WRITE fence at the end and
// waits for the fence.
//
//  - explicit render_target_path_d3d12 ("rtv" / "rov"), read by
//    D3D12RenderTargetCache::Initialize when the command processor starts;
//  - readback_resolve=full (d3d12_readback_resolve=true before canary): IssueCopy also copies the resolve result
//    into guest memory (otherwise it stays in the GPU's
//    shared memory).
//
// gpu::Shutdown runs (even if a REQUIRE fails in the constructor or in the
// test) before the arena and the log die: the guard is the last member.
// Note: a second D3D12Harness in the same process (two TEST_CASEs without a
// filter) still crashes with SIGSEGV in the second constructor even with
// ShutdownMemory at closing: re-initializing D3D12/Xenia in the
// same process is not supported (one test case per invocation, ctest runs them
// separately by name).
// The caller does SKIP without a hardware adapter before constructing it.
#pragma once

#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include "gpu/d3d12_backend.h"
#include "gpu/gpu_system.h"
#include "gpu/xenos.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/threads.h"
#include "log.h"
#include "physical_watch.h"
#include "pm4_builder.h"
#include "temp_dir.h"

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);

namespace gow2_test {

class D3D12Harness {
 public:
  static constexpr uint32_t kRingDwords = 0x8000 / 4;  // VdInitializeRingBuffer(.., 12)

  static uint32_t Get32(uint32_t a) { return *kernel::GuestPtr<be<uint32_t>>(a); }
  static void Put32(uint32_t a, uint32_t v) { *kernel::GuestPtr<be<uint32_t>>(a) = v; }
  // DWORD read by the host (little-endian) without conversions.
  static uint32_t HostGet32(uint32_t a) { return *kernel::GuestPtr<uint32_t>(a); }
  static void HostPut32(uint32_t a, uint32_t v) { *kernel::GuestPtr<uint32_t>(a) = v; }

  template <class F>
  static bool Eventually(F cond, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!cond()) {
      if (std::chrono::steady_clock::now() > end) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  static int64_t SummaryValue(std::string_view key) {
    const std::string s = kernel::stats::Summary();
    const std::string needle = "[summary] " + std::string(key) + "=";
    const auto pos = s.find(needle);
    return pos == std::string::npos ? -1 : std::stoll(s.substr(pos + needle.size()));
  }

  explicit D3D12Harness(std::string_view render_target_path) : dir_("gow2_d3d12_harness") {
    LogInit(dir_.path / "gow2.log");
    log_.open = true;
    REQUIRE(mem_.Init());
    kernel::g_base = mem_.base;
    REQUIRE(kernel::InitMemory(mem_));
    REQUIRE(kernel::InitThreads(mem_, &NoFunction, 0x40000));
    physwatch::Init(mem_);

    REQUIRE(gpu::SetXeniaCvar("render_target_path_d3d12", render_target_path));
    REQUIRE(gpu::SetXeniaCvar("readback_resolve", "full"));
    // canary creates the pipelines in background threads and skips draws until they
    // are ready: in the tests every draw must be executed.
    REQUIRE(gpu::SetXeniaCvar("async_shader_compilation", "false"));
    gpu::D3D12Options options;  // window nullptr: no presenter
    options.start = std::chrono::steady_clock::now();
    // The backend does not touch the shaders hook: tests that want it set it themselves.
    options.enhanced_shaders = false;
    auto backend = gpu::CreateD3D12Backend(mem_, options);
    REQUIRE(backend);
    REQUIRE(gpu::Init(mem_, std::move(backend)));
    guard_.active = true;

    ring_ = kernel::Memory()->phys.Alloc(0x8000, 0x1000);
    wb_ = kernel::Memory()->phys.Alloc(0x1000, 0x1000);
    fence_ = kernel::Memory()->phys.Alloc(0x1000, 0x1000);
    REQUIRE((ring_ && wb_ && fence_));
    Call(__imp__VdInitializeRingBuffer, ring_ & 0x1FFFFFFF, 12);
    Call(__imp__VdEnableRingBufferRPtrWriteBack, wb_ & 0x1FFFFFFF, 6);

    // Warm-up: the first block waits for the command processor's SetupContext
    // (up to 60 s on a cold cache), then the tests' limits apply.
    Put32(wb_, 0xFFFFFFFF);
    Pm4Builder nop;
    nop.SetRegs(gpu::kRegScratchReg0, {0});
    REQUIRE(Run(nop, std::chrono::milliseconds(60000)));
  }

  ~D3D12Harness() = default;  // the members: gpu::Shutdown, arena, log, folder

  D3D12Harness(const D3D12Harness&) = delete;
  D3D12Harness& operator=(const D3D12Harness&) = delete;

  GuestMemory& mem() { return mem_; }

  // Physical memory (guest address in the 0xA0000000 view or similar).
  static uint32_t AllocPhys(uint32_t size, uint32_t align = 0x1000) {
    return kernel::Memory()->phys.Alloc(size, align);
  }

  // Copies the packets into the ring with a fence at the end, sends them (CP_RB_WPTR)
  // and waits for the fence and the read pointer write-back within limit.
  bool Run(const Pm4Builder& packets, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
    Pm4Builder fence;
    fence.Fence(fence_, ++fence_value_);
    const auto& body = packets.words();
    const auto& tail = fence.words();
    REQUIRE(wptr_ + body.size() + tail.size() < kRingDwords);  // no ring wrap
    for (uint32_t w : body) Put32(ring_ + wptr_++ * 4, w);
    for (uint32_t w : tail) Put32(ring_ + wptr_++ * 4, w);
    Put32(gpu::kRegisterBase + gpu::kRegCpRbWptr * 4, wptr_);
    const uint32_t value = fence_value_, wptr = wptr_;
    return Eventually([&] { return Get32(fence_) == value && Get32(wb_) == wptr; }, limit);
  }

  // Stops the GPU (gpu::Shutdown) and returns the test log.
  std::string StopAndReadLog() {
    if (guard_.active) {
      gpu::Shutdown();
      guard_.active = false;
    }
    kernel::ShutdownMemory();  // before the arena: ~VirtualAllocator uses GuestMemory
    LogInit({});
    log_.open = false;
    std::ifstream f(dir_.path / "gow2.log");
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }

 private:
  static PPCFunc* NoFunction(uint32_t) { return nullptr; }

  static void Call(PPCFunc* fn, uint32_t a, uint32_t b) {
    PPCContext ctx{};
    ctx.r3.u64 = a;
    ctx.r4.u64 = b;
    ctx.lr = 0x82001234;
    fn(ctx, kernel::g_base);
  }

  struct LogGuard {
    bool open = false;
    ~LogGuard() {
      if (open) LogInit({});
    }
  };
  struct GpuGuard {
    bool active = false;
    ~GpuGuard() {
      if (active) gpu::Shutdown();
    }
  };

  // Reverse destruction order: guard (gpu::Shutdown), arena, log, folder.
  TempDir dir_;
  LogGuard log_;
  GuestMemory mem_;
  GpuGuard guard_;
  uint32_t ring_ = 0, wb_ = 0, fence_ = 0;
  uint32_t wptr_ = 0, fence_value_ = 0;
};

}  // namespace gow2_test
