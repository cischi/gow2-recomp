// gow2 --gpu d3d12 backend (gpu::CreateD3D12Backend): Xenia's D3D12CommandProcessor
// on the guest ring, DXGI presenter on an invisible window,
// screenshot. Real GuestMemory arena with the kernel initialized and physwatch
// connected, as in the game; no game bytes.
//
// IssueSwap presents on every XE_SWAP even when the front buffer was never
// resolved or the texture cache rejects it: host_frames_presented =
// number of VdSwaps executed. Device removed (ID3D12Device5::RemoveDevice) in
// a child process: exit 5 with the [gpu] lines and the summary.
//
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/d3d12_backend.h"
#include "gpu/d3d12_probe.h"
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
#include "temp_dir.h"

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSwap);

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

uint32_t g_stack = 0;  // arguments beyond r10 (r1 + 0x54 + (i-8)*8)

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  ctx.r1.u64 = g_stack;
  size_t i = 0;
  for (uint64_t a : args) {
    if (i < 8)
      *regs[i] = a;
    else
      Put32(g_stack + 0x54 + uint32_t(i - 8) * 8, uint32_t(a));
    ++i;
  }
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

PPCFunc* Resolve(uint32_t) { return nullptr; }

int64_t SummaryValue(const std::string& key) {
  const std::string s = stats::Summary();
  const std::string needle = "[summary] " + key + "=";
  const auto pos = s.find(needle);
  return pos == std::string::npos ? -1 : std::stoll(s.substr(pos + needle.size()));
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

uint32_t Le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

}  // namespace

TEST_CASE("d3d12_backend: VdSwap with a front buffer never resolved or rejected by the texture cache -> one frame each",
          "[d3d12_backend]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  INFO("adapter: " << *adapter);
  const uint32_t antialiasing = 1;  // fxaa (the backend is not recreated twice in the same process)

  gow2_test::TempDir dir("gow2_d3d12_backend");
  std::filesystem::create_directories(dir.path);
  LogInit(dir.path / "gow2.log");

  GuestMemory mem;
  REQUIRE(mem.Init());
  g_base = mem.base;
  REQUIRE(InitMemory(mem));
  REQUIRE(InitThreads(mem, &Resolve, 0x40000));
  physwatch::Init(mem);
  g_stack = mem.Alloc(0x1000, 0x10);
  REQUIRE(g_stack);

  plat::Window* window = plat::CreateGameWindow("gow2 d3d12 backend test", {1280, 720}, false, [] {});
  REQUIRE(window);
  struct WindowGuard {
    plat::Window* w;
    ~WindowGuard() { plat::DestroyGameWindow(w); }
  } window_guard{window};

  // The game's EDRAM path (main.cpp, Global Constraints).
  REQUIRE(gpu::SetXeniaCvar("render_target_path_d3d12", "rtv"));
  const auto shot = dir.path / "s.bmp";
  gpu::D3D12Options options;
  options.window = window;
  options.screenshots = {gpu::ScreenshotRequest{0, shot}};
  options.start = Clock::now();
  options.antialiasing = antialiasing;
  // gow2_dlss.dll is not next to the test (the tests' real bridge is in real_dlss/): DLSS unavailable,
  // played as with off, FXAA included.
  options.upscaler = 2;
  auto backend = gpu::CreateD3D12Backend(mem, options);
  REQUIRE(backend);
  REQUIRE(gpu::Init(mem, std::move(backend)));
  // The GPU stops before the window and the arena (even if a REQUIRE fails).
  struct GpuGuard {
    ~GpuGuard() { gpu::Shutdown(); }
  } gpu_guard;

  const uint32_t ring = Memory()->phys.Alloc(0x8000, 0x1000);
  const uint32_t wb = Memory()->phys.Alloc(0x1000, 0x1000);
  const uint32_t data = Memory()->phys.Alloc(0x1000, 0x1000);
  // 1280x720 32-bit front buffer, never written or resolved by the GPU. Tiled:
  // 1280x736 (rows in 32-row tiles) = 0x398000 bytes, more than 1280*720*4; the
  // texture cache rejects a texture beyond the end of physical memory and
  // the physical allocator starts from the top: allocate with a margin.
  const uint32_t frontbuffer = Memory()->phys.Alloc(0x400000, 0x1000);
  REQUIRE((ring && wb && data && frontbuffer));
  Call(__imp__VdInitializeRingBuffer, {ring & 0x1FFFFFFF, 12});
  Call(__imp__VdEnableRingBufferRPtrWriteBack, {wb & 0x1FFFFFFF, 6});

  // The CP's SetupContext on a cold driver cache can exceed 2 s: a NOP
  // first (up to 60 s), then the test's limit.
  uint32_t wptr = 0;
  Put32(wb, 0xFFFFFFFF);
  for (uint32_t w : {MakePacketType3(gpu::kPm4Nop, 1), 0u}) Put32(ring + wptr++ * 4, w);
  Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  REQUIRE(Eventually([&] { return Get32(wb) == wptr; }, 60000ms));

  // VdSwap parameters like the game: fetch constant of a tiled
  // k_8_8_8_8 1280x720 2D texture (xenos.h xe_gpu_texture_fetch_t): type 2, pitch
  // 1280 >> 5, tiled; format 6, endian 8in32; swizzle RGBA; 2D dimension.
  const uint32_t fetch = data, fb_ptr = data + 0x40, fmt = data + 0x44, cs = data + 0x48, w_ptr = data + 0x4C,
                 h_ptr = data + 0x50;
  const uint32_t fetch_words[6] = {0x2u | (40u << 22) | (1u << 31), (frontbuffer & 0xFFFFF000) | 0x86,
                                   1279u | (719u << 13), 0x688u << 1, 0, 1u << 9};
  for (int i = 0; i < 6; ++i) Put32(fetch + i * 4, fetch_words[i]);
  Put32(fb_ptr, frontbuffer);
  Put32(fmt, 0x6);
  Put32(cs, 0);
  Put32(w_ptr, 1280);
  Put32(h_ptr, 720);

  const int64_t presented_before = SummaryValue("host_frames_presented");
  REQUIRE(presented_before >= 0);
  const auto start = Clock::now();
  for (int swap = 0; swap < 3; ++swap) {
    Call(__imp__VdSwap, {ring + wptr * 4, fetch, 0, 0, 0, fb_ptr, fmt, cs, w_ptr, h_ptr});
    wptr += 64;  // 64 DWORDs reserved in the ring
  }
  Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  CHECK(Eventually([&] { return SummaryValue("host_frames_presented") == presented_before + 3; }, 2000ms));
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  WARN("3 swaps presented in " << ms << " ms");
  CHECK(Get32(wb) == wptr);
  std::this_thread::sleep_for(50ms);
  CHECK(SummaryValue("host_frames_presented") == presented_before + 3);  // not more
  // The front buffer was loaded by Xenia's texture cache (IssueSwap ->
  // RequestSwapTexture), which observes its pages through xe::Memory ->
  // physwatch; a CPU write invalidates and reopens them.
  CHECK(physwatch::IsWatched(frontbuffer & 0x1FFFFFFF));
  Put32(frontbuffer, 0x12345678);
  CHECK_FALSE(physwatch::IsWatched(frontbuffer & 0x1FFFFFFF));
  CHECK(Get32(frontbuffer) == 0x12345678);

  // Front buffer that the texture cache rejects (base address 0): IssueSwap
  // returns without RefreshGuestOutput, but the frame is presented anyway
  // (the last output).
  Put32(fetch + 4, 0x86);
  Call(__imp__VdSwap, {ring + wptr * 4, fetch, 0, 0, 0, fb_ptr, fmt, cs, w_ptr, h_ptr});
  wptr += 64;
  Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, wptr);
  CHECK(Eventually([&] { return SummaryValue("host_frames_presented") == presented_before + 4; }, 2000ms));
  CHECK(Get32(wb) == wptr);

  const auto bmp = ReadAll(shot);
  REQUIRE(bmp.size() == 54 + 1280 * 720 * 3);
  CHECK(bmp[0] == 'B');
  CHECK(bmp[1] == 'M');
  CHECK(Le32(bmp, 18) == 1280);
  CHECK(Le32(bmp, 22) == 720);

  gpu::Shutdown();
  LogInit({});
  std::ifstream log_file(dir.path / "gow2.log");
  const std::string log{std::istreambuf_iterator<char>(log_file), std::istreambuf_iterator<char>()};
  INFO(log);
  CHECK(log.find("device removed") == std::string::npos);
  CHECK(log.find("[gpu] screenshot ") != std::string::npos);
  CHECK(log.find("antialiasing fxaa") != std::string::npos);
  CHECK(log.find("DLSS unavailable") != std::string::npos);
  CHECK(log.find("upscaler dlss") == std::string::npos);
  CHECK(log.find("FXAA and CAS ignored") == std::string::npos);
}

TEST_CASE("d3d12_backend: device removed -> exit 5, [gpu] device removed, [gpu] fatal and summary",
          "[d3d12_backend]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  const auto log = gow2_test::UniqueTempPath("gow2_device_removed") += ".log";
  const std::string cmd =
      "\"\"" + std::string(GOW2_D3D12_DEVICE_REMOVED_CHILD) + "\" \"" + log.string() + "\" >nul 2>&1\"";
  const int rc = std::system(cmd.c_str());
  std::ifstream in(log);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(log);
  const std::string text = ss.str();
  INFO(text);
  CHECK(rc == 5);
  CHECK(text.find("child: RemoveDevice\n") != std::string::npos);
  const auto removed = text.find("[gpu] device removed reason=0x");
  REQUIRE(removed != std::string::npos);
  const uint32_t reason = uint32_t(std::stoul(text.substr(removed + 30, 8), nullptr, 16));
  CHECK(text.find("[gpu] device removed reason=0x", removed + 1) == std::string::npos);  // only once
  CHECK(reason != 0);  // DXGI_ERROR_DEVICE_REMOVED or similar
  const auto fatal = text.find("[gpu] fatal: ");
  CHECK(fatal != std::string::npos);
  CHECK(fatal > removed);
  // from the command processor (BeginSubmission -> OnHostGpuLossFromAnyThread), not from the Present
  CHECK(text.find("[gpu] fatal: D3D12 device lost") == fatal);
  const auto summary = text.find("[summary] host_frames_presented=");
  CHECK(summary != std::string::npos);
  CHECK(summary > fatal);
  CHECK(text.find("[summary] gpu_draws=") != std::string::npos);
  CHECK(text.find("no exit") == std::string::npos);
}

// The Xenia cvars that main.cpp and the tests set must exist in the
// vendored code: a name that disappeared or was renamed (for example by changing
// Xenia commit) makes SetXeniaCvar fail, it is not ignored.
TEST_CASE("d3d12_backend: the Xenia cvars set by gow2 exist", "[gpu][d3d12]") {
  for (const auto& [name, value] : {std::pair{"render_target_path_d3d12", "rtv"},
                                    std::pair{"readback_resolve", "none"},
                                    std::pair{"async_shader_compilation", "true"},
                                    std::pair{"draw_resolution_scale_x", "1"},
                                    std::pair{"draw_resolution_scale_y", "1"},
                                    std::pair{"draw_resolution_scaled_texture_offsets", "true"},
                                    std::pair{"d3d12_submit_every_draws", "100"},
                                    std::pair{"gow2_clear_msaa_remap", "true"},
                                    std::pair{"gow2_resolve_fill_depth", "false"},
                                    std::pair{"draw_resolution_scale_threshold", "640"}}) {
    INFO(name);
    CHECK(gpu::SetXeniaCvar(name, value));
  }
  CHECK_FALSE(gpu::SetXeniaCvar("nonexistent_gow2_cvar", "1"));
}
