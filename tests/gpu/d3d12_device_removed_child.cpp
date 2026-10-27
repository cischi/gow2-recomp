// Child process of d3d12_backend_test: the game loses the D3D12 device.
// Prepares the --gpu d3d12 backend like main.cpp (arena, kernel, physwatch,
// invisible window, xe::FatalError hook that writes the summary), then
// really removes the device with ID3D12Device5::RemoveDevice and sends an
// XE_SWAP: the command processor notices in BeginSubmission
// (OnHostGpuLossFromAnyThread) and the process must exit with 5 after
// `[gpu] device removed reason=...`, `[gpu] fatal: ...` and the summary.
// No Catch2: the exit is done by _Exit from the command processor thread.
//
//   d3d12_device_removed_child <log file>
// Exit 5 expected; 1 if the environment cannot be prepared; 2 if the process does not exit
// within 20 s of the device removal.
#include "kernel/registry.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include <d3d12.h>
#include <wrl/client.h>

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
#include "platform/platform.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);

namespace {

using namespace kernel;
using namespace std::chrono_literals;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }

void Call(PPCFunc* fn, uint32_t a, uint32_t b) {
  PPCContext ctx{};
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
}

PPCFunc* Resolve(uint32_t) { return nullptr; }

// Like OnGpuFatalError of main.cpp.
void OnGpuFatalError() {
  LogLine("gow2: fatal GPU error (exit 5); to start the game without a renderer: --gpu fake");
  stats::WriteSummary();
}

int Fail(const char* what) {
  LogLine(std::string("child: ") + what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  LogInit(argv[1]);
  static GuestMemory mem;
  if (!mem.Init()) return Fail("arena");
  g_base = mem.base;
  if (!InitMemory(mem) || !InitThreads(mem, &Resolve, 0x40000)) return Fail("kernel");
  physwatch::Init(mem);
  gpu::SetFatalErrorHook(&OnGpuFatalError);
  plat::Window* window = plat::CreateGameWindow("gow2 device removed", {1280, 720}, false, [] {});
  if (!window) return Fail("window");
  gpu::D3D12Options options;
  options.window = window;
  options.start = std::chrono::steady_clock::now();
  auto backend = gpu::CreateD3D12Backend(mem, options);
  if (!backend || !gpu::Init(mem, std::move(backend))) return Fail("backend");

  const uint32_t ring = Memory()->phys.Alloc(0x8000, 0x1000);
  const uint32_t wb = Memory()->phys.Alloc(0x1000, 0x1000);
  if (!ring || !wb) return Fail("physical memory");
  Call(__imp__VdInitializeRingBuffer, ring & 0x1FFFFFFF, 12);
  Call(__imp__VdEnableRingBufferRPtrWriteBack, wb & 0x1FFFFFFF, 6);
  // CP ready (SetupContext can take long on a cold cache).
  uint32_t wptr = 0;
  Put32(wb, 0xFFFFFFFF);
  Put32(ring + wptr++ * 4, gpu::MakePacketType3(gpu::kPm4Nop, 1));
  Put32(ring + wptr++ * 4, 0);
  Put32(gpu::kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  const auto ready_by = std::chrono::steady_clock::now() + 60s;
  while (Get32(wb) != wptr) {
    if (std::chrono::steady_clock::now() > ready_by) return Fail("CP not ready in 60 s");
    std::this_thread::sleep_for(1ms);
  }

  // D3D12CreateDevice returns the same device per adapter and process: a
  // second provider gives the backend's device, which is really removed.
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  if (!provider) return Fail("provider");
  Microsoft::WRL::ComPtr<ID3D12Device5> device5;
  if (FAILED(provider->GetDevice()->QueryInterface(IID_PPV_ARGS(&device5)))) return Fail("ID3D12Device5");
  LogLine("child: RemoveDevice");
  device5->RemoveDevice();

  // XE_SWAP: IssueSwap -> BeginSubmission sees the removed device.
  for (uint32_t w : {gpu::MakePacketType3(gpu::kPm4XeSwap, 4), gpu::kSwapSignature, 0u, 1280u, 720u})
    Put32(ring + wptr++ * 4, w);
  Put32(gpu::kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  std::this_thread::sleep_for(20s);
  LogLine("child: no exit 20 s after RemoveDevice");
  std::_Exit(2);
}
