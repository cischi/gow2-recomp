// Vendored Xenia D3D12 backend (xenia_gpu_d3d12): D3D12Provider with
// the gow2 patch (no presenter, no dxilconv/dxcompiler) and
// D3D12CommandProcessor created by gpu::CreateXeniaBackend on a real
// GuestMemory arena with the kernel initialized, like tests/gpu/xenia_cp_test.cpp.
// Presenter nullptr; the physical memory observation callbacks stay no-ops.
//
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter;
// WARP does not count): D3D12Provider::Create would call xe::FatalError.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>

#include "gpu/d3d12_probe.h"
#include "gpu/gpu_system.h"
#include "gpu/xenia_backend.h"
#include "gpu/xenos.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/threads.h"
#include "xenia/base/cvar.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/memory.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);

namespace {

using namespace kernel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using gpu::kRegisterBase;
using gpu::MakePacketType3;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }

template <class F>
bool Eventually(F cond, std::chrono::milliseconds limit) {
  const auto end = Clock::now() + limit;
  while (!cond()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

PPCFunc* Resolve(uint32_t) { return nullptr; }

uint32_t Phys(uint32_t guest, uint32_t endian = 2) { return (guest & 0x1FFFFFFF) | endian; }

void SkipWithoutHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  INFO("adapter: " << *adapter);
}

}  // namespace

TEST_CASE("d3d12_cp: D3D12Provider::Create with device and direct queue", "[d3d12_cp]") {
  SkipWithoutHardwareAdapter();
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  CHECK(provider->GetDevice() != nullptr);
  CHECK(provider->GetDirectQueue() != nullptr);
  CHECK(provider->GetDXGIFactory() != nullptr);
  CHECK(provider->GetViewDescriptorSize() > 0);
  // dxilconv.dll and dxcompiler.dll are not loaded (patch 0006)
  void* object = nullptr;
  CHECK(provider->DxbcConverterCreateInstance(CLSID_DxbcConverter, __uuidof(IDxbcConverter), &object) ==
        E_NOINTERFACE);
  CHECK(provider->DxcCreateInstance(CLSID_DxcUtils, __uuidof(IDxcUtils), &object) == E_NOINTERFACE);
  CHECK(object == nullptr);
  CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
}

TEST_CASE("d3d12_cp: D3D12CommandProcessor executes MEM_WRITE and EVENT_WRITE_SHD from the ring, Shutdown within 2 s",
          "[d3d12_cp]") {
  SkipWithoutHardwareAdapter();
  // The game's EDRAM path (main.cpp, Global Constraints).
  REQUIRE(gow2::xenia::SetCvar("render_target_path_d3d12", "rtv"));
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);

  GuestMemory mem;
  REQUIRE(mem.Init());
  g_base = mem.base;
  REQUIRE(InitMemory(mem));
  REQUIRE(InitThreads(mem, &Resolve, 0x40000));
  auto xenia_memory = std::make_unique<xe::Memory>(mem.base);

  xe::gpu::d3d12::D3D12CommandProcessor* cp = nullptr;
  auto graphics_system =
      std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>(xenia_memory.get(), provider.get(), nullptr);
  CHECK(graphics_system->provider() == provider.get());
  CHECK(graphics_system->presenter() == nullptr);
  auto backend = gpu::CreateXeniaBackend(
      std::move(graphics_system),
      [&cp](xe::gpu::GraphicsSystem* gs) -> std::unique_ptr<xe::gpu::CommandProcessor> {
        auto p = std::make_unique<xe::gpu::d3d12::D3D12CommandProcessor>(
            static_cast<xe::gpu::d3d12::D3D12GraphicsSystem*>(gs), gs->kernel_state());
        cp = p.get();
        return p;
      });
  REQUIRE(backend);
  REQUIRE(cp);
  REQUIRE(gpu::Init(mem, std::move(backend)));

  const uint32_t ring = Memory()->phys.Alloc(0x8000, 0x1000);
  const uint32_t wb = Memory()->phys.Alloc(0x1000, 0x1000);
  const uint32_t data = Memory()->phys.Alloc(0x1000, 0x1000);
  REQUIRE((ring && wb && data));
  Call(__imp__VdInitializeRingBuffer, {ring & 0x1FFFFFFF, 12});
  Call(__imp__VdEnableRingBufferRPtrWriteBack, {wb & 0x1FFFFFFF, 6});

  // The CP thread runs SetupContext (device, cache, internal pipelines)
  // before reading the ring; with a cold driver cache (first run)
  // it can take more than 2 s. A first block with a NOP waits for the CP to be
  // ready; the test's 2 s count from the real block.
  for (uint32_t i = 0; i < 4; ++i) Put32(data + i * 4, 0xFFFFFFFF);
  Put32(wb, 0xFFFFFFFF);
  uint32_t wptr = 0;
  for (uint32_t w : {MakePacketType3(gpu::kPm4Nop, 1), 0u}) Put32(ring + wptr++ * 4, w);
  const auto setup_start = Clock::now();
  Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  REQUIRE(Eventually([&] { return Get32(wb) == wptr; }, 60000ms));
  const double setup_ms = std::chrono::duration<double, std::milli>(Clock::now() - setup_start).count();
  WARN("CP ready in " << setup_ms << " ms");
  std::this_thread::sleep_for(60ms);  // a few vblanks for the counter
  const auto start = Clock::now();
  for (uint32_t w : {MakePacketType3(gpu::kPm4MemWrite, 3), Phys(data), 0xF00Du, 0xBEEFu,
                     // without bit 31: the packet's value
                     MakePacketType3(gpu::kPm4EventWriteShd, 3), 0x00000007u, Phys(data + 8), 0x1234u,
                     // with bit 31: the vblank counter
                     MakePacketType3(gpu::kPm4EventWriteShd, 3), 0x80000007u, Phys(data + 12), 0x1234u}) {
    Put32(ring + wptr * 4, w);
    ++wptr;
  }
  Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);

  CHECK(Eventually([&] { return Get32(wb) == wptr; }, 2000ms));
  const double block_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  WARN("block executed in " << block_ms << " ms");
  CHECK(Get32(data) == 0xF00D);
  CHECK(Get32(data + 4) == 0xBEEF);
  CHECK(Get32(data + 8) == 0x1234);
  const uint32_t vblanks = Get32(data + 12);
  INFO("vblank counter " << vblanks);
  CHECK(vblanks != 0x1234);
  CHECK(vblanks >= 1);
  CHECK(vblanks != 0xFFFFFFFF);

  const auto t0 = Clock::now();
  gpu::Shutdown();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("Shutdown in " << ms << " ms");
  CHECK(ms < 2000.0);
  CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
}
