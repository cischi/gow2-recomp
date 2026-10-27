// DLSS adapter (runtime/gpu/upscaler_dlss.h): Halton jitter, loading of gow2_dlss.dll and
// forwarding of frames, with a fake bridge (tests/gpu/fake_dlss_bridge.cpp) in GOW2_FAKE_DLSS_DIR; with the real
// bridge in GOW2_REAL_DLSS_DIR (build with GOW2_DLSS_SDK_DIR) also DLSS on an NVIDIA GPU, otherwise SKIP.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gpu/d3d12_probe.h"
#include "gpu/dlss_bridge/gow2_dlss_bridge.h"
#include "gpu/dlss_bridge/gow2_dlss_policy.h"
#include "gpu/dlss_bridge_loader.h"
#include "gpu/upscaler.h"
#include "gpu/upscaler_dlss.h"
#include "settings.h"
#include "log.h"
#include "gpu_texture_util.h"
#include "temp_dir.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace {

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

float Halton(uint32_t i, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  return r;
}

// Environment variable of the fake bridge for the duration of a block.
struct ScopedEnv {
  const char* name;
  ScopedEnv(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
  ~ScopedEnv() { SetEnvironmentVariableA(name, nullptr); }
};

const std::filesystem::path kFakeDir = GOW2_FAKE_DLSS_DIR;

const std::filesystem::path kRealDir = GOW2_REAL_DLSS_DIR;

// Creates the upscaler with the fake bridge and also returns the log written during creation.
std::unique_ptr<gpu::Upscaler> CreateWithLog(const std::filesystem::path& dir, std::string& log,
                                             settings::DlssPreset preset = settings::DlssPreset::kDefault) {
  gow2_test::TempDir tmp("upscaler_dlss");
  LogInit(tmp.path / "log.txt");
  auto u = gpu::CreateDlssUpscaler(nullptr, 1280, 720, 2560, 1440, dir, preset);
  LogInit({});
  log = ReadText(tmp.path / "log.txt");
  return u;
}

}  // namespace

TEST_CASE("dlss: Gow2DlssQualityFor DLAA at ratio 1 and thresholds", "[dlss]") {
  using Q = gow2_dlss::Quality;
  CHECK(gow2_dlss::QualityFor(1280, 1280) == Q::kDlaa);
  CHECK(gow2_dlss::QualityFor(1280, 1200) == Q::kDlaa);  // output < render (small window)
  CHECK(gow2_dlss::QualityFor(1280, 1920) == Q::kMaxQuality);
  CHECK(gow2_dlss::QualityFor(1280, 2304) == Q::kBalanced);  // 1.8 (< 1.85, the Balanced/Performance boundary)
  CHECK(gow2_dlss::QualityFor(1280, 2560) == Q::kMaxPerf);
  CHECK(gow2_dlss::QualityFor(1280, 3840) == Q::kUltraPerf);
  CHECK(gow2_dlss::QualityFor(0, 1280) == Q::kDlaa);
}

TEST_CASE("dlss: NGX preset from DlssPreset", "[dlss]") {
  using P = settings::DlssPreset;
  CHECK(gpu::NgxPreset(P::kDefault) == 0);
  CHECK(gpu::NgxPreset(P::kE) == 5);
  CHECK(gpu::NgxPreset(P::kF) == 6);
  CHECK(gpu::NgxPreset(P::kJ) == 10);
  CHECK(gpu::NgxPreset(P::kK) == 11);
  CHECK(gpu::NgxPreset(P::kM) == 13);
  CHECK(gpu::NgxPreset(P::kL) == 12);
}

TEST_CASE("dlss: preset passed to the bridge", "[dlss]") {
  std::string log;
  auto u = CreateWithLog(kFakeDir, log, settings::DlssPreset::kM);
  REQUIRE(u);
  HMODULE dll = GetModuleHandleW(L"gow2_dlss.dll");
  REQUIRE(dll);
  const auto last = reinterpret_cast<int (*)()>(GetProcAddress(dll, "FakeDlssLastPreset"));
  CHECK(last() == 13);
  CHECK(log.find("ABI 1") == std::string::npos);
  u.reset();
  u = CreateWithLog(kFakeDir, log);
  REQUIRE(u);
  CHECK(last() == 0);
}

TEST_CASE("dlss: ABI 1 bridge, preset not available", "[dlss]") {
  ScopedEnv abi("GOW2_FAKE_DLSS_ABI", "1");
  std::string log;
  auto u = CreateWithLog(kFakeDir, log, settings::DlssPreset::kM);
  REQUIRE(u);  // SR stays
  CHECK(log.find("[gpu] DLSS: ABI 1 bridge: preset not available") != std::string::npos);
  u.reset();
  CreateWithLog(kFakeDir, log);  // default preset: no line
  CHECK(log.find("ABI 1 bridge") == std::string::npos);
}

TEST_CASE("dlss: CreateEx receives the neural rendering fields at zero (ABI 5)", "[dlss]") {
  auto u = gpu::CreateDlssUpscaler(nullptr, 1280, 720, 2560, 1440, kFakeDir, settings::DlssPreset::kM);
  REQUIRE(u);
  HMODULE dll = GetModuleHandleW(L"gow2_dlss.dll");
  REQUIRE(dll);
  const auto cfg = reinterpret_cast<const Gow2DlssConfig* (*)()>(GetProcAddress(dll, "FakeDlssLastConfig"));
  REQUIRE((cfg != nullptr));
  CHECK(cfg()->preset == 13);
  CHECK(cfg()->reserved_nr0 == 0);
  CHECK(cfg()->reserved_nr1 == 0.0f);
  CHECK(cfg()->reserved_nr2 == 0);
}

TEST_CASE("dlss: jitter phases like the NVIDIA guide", "[dlss]") {
  CHECK(gpu::DlssJitterPhaseCount(1280, 1280) == 8);
  CHECK(gpu::DlssJitterPhaseCount(1280, 2560) == 32);
  CHECK(gpu::DlssJitterPhaseCount(1280, 3840) == 72);
  CHECK(gpu::DlssJitterPhaseCount(1280, 1920) == 18);
}

TEST_CASE("dlss: Halton (2, 3) jitter centered, index from 1", "[dlss]") {
  for (uint32_t i = 0; i < 32; ++i) {
    float x = 9.0f, y = 9.0f;
    gpu::DlssJitterOffset(i, 32, x, y);
    CHECK(x == Halton(i + 1, 2) - 0.5f);
    CHECK(y == Halton(i + 1, 3) - 0.5f);
    CHECK((x >= -0.5f && x < 0.5f));
    CHECK((y >= -0.5f && y < 0.5f));
  }
  float x0, y0, x1, y1;
  gpu::DlssJitterOffset(3, 32, x0, y0);
  gpu::DlssJitterOffset(35, 32, x1, y1);
  CHECK(x0 == x1);
  CHECK(y0 == y1);
}

TEST_CASE("dlss: not available without gow2_dlss.dll", "[dlss]") {
  gow2_test::TempDir empty("dlss_empty");
  std::filesystem::create_directories(empty.path);
  std::string log;
  CHECK_FALSE(CreateWithLog(empty.path, log));
  CHECK(log.find("[gpu] DLSS unavailable: ") != std::string::npos);
  CHECK(log.find("gow2_dlss.dll") != std::string::npos);
}

TEST_CASE("dlss: bridge with a different ABI rejected", "[dlss]") {
  ScopedEnv abi("GOW2_FAKE_DLSS_ABI", "99");
  std::string log;
  CHECK_FALSE(CreateWithLog(kFakeDir, log));
  CHECK(log.find("ABI") != std::string::npos);
}

TEST_CASE("dlss: bridge Create failed", "[dlss]") {
  ScopedEnv fail("GOW2_FAKE_DLSS_CREATE_FAIL", "1");
  std::string log;
  CHECK_FALSE(CreateWithLog(kFakeDir, log));
  CHECK(log.find("[gpu] DLSS: creation rejected by the fake") != std::string::npos);
}

// ABI 3 bridge (frame generation): loading shared with SR, FgQuery and the fake's variable.
TEST_CASE("dlss: LoadDlssBridge, ABI 5 bridge with FG available", "[dlss][fg]") {
  std::string why;
  const auto bridge = gpu::LoadDlssBridge(kFakeDir, why);
  REQUIRE(bridge);
  CHECK(bridge->api->abi == 5);
  REQUIRE(bool(bridge->api->FgQuery));
  Gow2DlssFgCaps caps{};
  CHECK(bridge->api->FgQuery(nullptr, kFakeDir.c_str(), &caps, [](const char*) {}) == 0);
  CHECK(caps.available == 1);
  CHECK(caps.multi_frame_count_max == 1);
  FreeLibrary(bridge->dll);
}

TEST_CASE("dlss: LoadDlssBridge, FG not available from the fake", "[dlss][fg]") {
  ScopedEnv fg("GOW2_FAKE_DLSS_FG", "0");
  std::string why;
  const auto bridge = gpu::LoadDlssBridge(kFakeDir, why);
  REQUIRE(bridge);
  Gow2DlssFgCaps caps{};
  caps.available = 1;
  CHECK(bridge->api->FgQuery(nullptr, kFakeDir.c_str(), &caps, [](const char*) {}) == 0);
  CHECK(caps.available == 0);
  FreeLibrary(bridge->dll);
}

TEST_CASE("dlss: LoadDlssBridge, ABI 2 bridge accepted without FG", "[dlss][fg]") {
  ScopedEnv abi("GOW2_FAKE_DLSS_ABI", "2");
  std::string why;
  const auto bridge = gpu::LoadDlssBridge(kFakeDir, why);
  REQUIRE(bridge);
  CHECK(bridge->api->abi < 3);
  FreeLibrary(bridge->dll);
}

TEST_CASE("dlss: LoadDlssBridge, out-of-range ABI rejected", "[dlss][fg]") {
  ScopedEnv abi("GOW2_FAKE_DLSS_ABI", "6");
  std::string why;
  CHECK_FALSE(gpu::LoadDlssBridge(kFakeDir, why));
  CHECK(why.find("ABI") != std::string::npos);
}

TEST_CASE("dlss: LoadDlssBridge, folder without a bridge", "[dlss][fg]") {
  gow2_test::TempDir empty("dlss_fg_empty");
  std::filesystem::create_directories(empty.path);
  std::string why;
  CHECK_FALSE(gpu::LoadDlssBridge(empty.path, why));
  CHECK(why.find((empty.path / "gow2_dlss.dll").string()) != std::string::npos);
}

TEST_CASE("dlss: the fake's FG, create/evaluate/destroy and counter", "[dlss][fg]") {
  std::string why;
  const auto bridge = gpu::LoadDlssBridge(kFakeDir, why);
  REQUIRE(bridge);
  const auto evaluations =
      reinterpret_cast<uint32_t (*)()>(GetProcAddress(bridge->dll, "FakeDlssFgEvaluations"));
  REQUIRE(bool(evaluations));
  const Gow2DlssFgConfig cfg{kFakeDir.c_str(), 2560, 1440, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, 0};
  void* ctx = bridge->api->FgCreate(nullptr, nullptr, &cfg, [](const char*) {});
  REQUIRE(ctx);
  CHECK(std::string(bridge->api->FgDescription(ctx)).size() > 0);
  const uint32_t before = evaluations();
  Gow2DlssFgFrame frame{};
  CHECK(bridge->api->FgEvaluate(ctx, &frame) == 0);
  CHECK(evaluations() == before + 1);
  bridge->api->FgDestroy(ctx);
  FreeLibrary(bridge->dll);
}

TEST_CASE("dlss: Dispatch forwards the frame to the bridge", "[dlss]") {
  std::string log;
  auto u = CreateWithLog(kFakeDir, log);
  REQUIRE(u);
  CHECK(u->Name() == "fake DLSS");
  CHECK(log.find("[gpu] DLSS: fake ready") != std::string::npos);
  CHECK(u->JitterPhaseCount(1280, 2560) == 32);

  auto* fake = reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1000));
  auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(0x10));
  const gpu::UpscaleFrame f{cmd, fake + 1, fake + 2, fake + 3, fake + 4, fake + 5, 1280, 720, 2560, 1440,
                            0.25f, -0.125f, 2.0f, 3.0f, true, -1.0f, 16.5f, false};
  REQUIRE(u->Dispatch(f));
  HMODULE dll = GetModuleHandleW(L"gow2_dlss.dll");
  REQUIRE(dll);
  using LastFn = const Gow2DlssFrame* (*)();
  const Gow2DlssFrame& got = *reinterpret_cast<LastFn>(GetProcAddress(dll, "FakeDlssLastFrame"))();
  CHECK(got.cmd == cmd);
  CHECK(got.color == fake + 1);
  CHECK(got.depth == fake + 2);
  CHECK(got.motion == fake + 3);
  CHECK(got.output == fake + 4);
  CHECK(got.bias_mask == fake + 5);
  CHECK(got.render_w == 1280);
  CHECK(got.render_h == 720);
  CHECK(got.output_w == 2560);
  CHECK(got.output_h == 1440);
  CHECK(got.jitter_x == 0.25f);
  CHECK(got.jitter_y == -0.125f);
  CHECK(got.mv_scale_x == 2.0f);
  CHECK(got.mv_scale_y == 3.0f);
  CHECK(got.frame_ms == 16.5f);
  CHECK(got.reset == 1);

  ScopedEnv bad("GOW2_FAKE_DLSS_EVAL_RESULT", "5");
  CHECK_FALSE(u->Dispatch(f));
}

// The context is recreated on every resize: the sharpness line once per process, not per context.
TEST_CASE("dlss: sharpness ignored, a single line even with several contexts", "[dlss]") {
  gow2_test::TempDir tmp("upscaler_dlss_sharp");
  LogInit(tmp.path / "log.txt");
  auto* fake = reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1000));
  const gpu::UpscaleFrame f{nullptr, fake, fake, fake, fake, fake, 1280, 720, 2560, 1440,
                            0.0f, 0.0f, 1.0f, 1.0f, false, 0.5f, 16.0f, false};
  for (int i = 0; i < 2; ++i) {
    auto u = gpu::CreateDlssUpscaler(nullptr, 1280, 720, 2560, 1440, kFakeDir);
    REQUIRE(u);
    CHECK(u->Dispatch(f));
    CHECK(u->Dispatch(f));
  }
  LogInit({});
  const std::string log = ReadText(tmp.path / "log.txt");
  const std::string line = "sharpness not supported";
  const size_t first = log.find(line);
  CHECK((first == std::string::npos || log.find(line, first + 1) == std::string::npos));
}

TEST_CASE("dlss: real bridge on NVIDIA GPU", "[dlss]") {
  if (!std::filesystem::exists(kRealDir / "gow2_dlss.dll")) SKIP("gow2_dlss.dll not in the build (GOW2_DLSS_SDK_DIR)");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  if (provider->GetAdapterVendorID() != xe::ui::GraphicsProvider::GpuVendorID::kNvidia) SKIP("adapter not NVIDIA");
  ID3D12Device* device = provider->GetDevice();

  // Repeated Init and Shutdown on the same device (probe at startup, then the presenter).
  REQUIRE(gpu::CreateDlssUpscaler(device, 640, 360, 1280, 720, kRealDir));
  auto dlss = gpu::CreateDlssUpscaler(device, 640, 360, 1280, 720, kRealDir);
  REQUIRE(dlss);
  CHECK(dlss->Name() == "DLSS 310.9.16");

  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto color = gow2_test::MakeTexture(device, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, kRead);
  auto depth = gow2_test::MakeTexture(device, 640, 360, DXGI_FORMAT_R32_FLOAT, kRead);
  auto motion = gow2_test::MakeTexture(device, 640, 360, DXGI_FORMAT_R16G16_FLOAT, kRead);
  std::vector<uint8_t> rgba(640 * 360 * 4);
  for (uint32_t y = 0; y < 360; ++y)
    for (uint32_t x = 0; x < 640; ++x) {
      uint8_t* p = &rgba[(y * 640 + x) * 4];
      p[0] = uint8_t(x * 255 / 639), p[1] = uint8_t(y * 255 / 359), p[2] = 128, p[3] = 255;
    }
  gow2_test::Upload(*provider, color.Get(), 4, rgba.data(), kRead);
  const std::vector<float> half(640 * 360, 0.5f);
  gow2_test::Upload(*provider, depth.Get(), 4, half.data(), kRead);
  const std::vector<uint32_t> zero_mv(640 * 360, 0);
  gow2_test::Upload(*provider, motion.Get(), 4, zero_mv.data(), kRead);

  // Outputs of different sizes: each recreates the feature (GPU idle between them, like in the presenter).
  // Also 1:1 (window below the game's resolution: the presenter asks for output = render).
  for (auto [ow, oh] : {std::pair{1280u, 720u}, std::pair{960u, 540u}, std::pair{640u, 360u}}) {
    auto output = gow2_test::MakeTexture(device, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, kUav,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const uint32_t phases = dlss->JitterPhaseCount(640, ow);
    gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
      for (uint32_t frame = 0; frame < 4; ++frame) {
        gpu::UpscaleFrame f{};
        f.cmd = l;
        f.color = color.Get(), f.depth = depth.Get(), f.motion = motion.Get(), f.output = output.Get();
        f.render_w = 640, f.render_h = 360, f.output_w = ow, f.output_h = oh;
        dlss->JitterOffset(frame, phases, f.jitter_x, f.jitter_y);
        f.mv_scale_x = f.mv_scale_y = 1.0f;
        f.reset = frame == 0;
        f.sharpness = -1.0f;
        f.frame_ms = 16.6f;
        CHECK(dlss->Dispatch(f));
      }
    });
    const auto out = gow2_test::Readback(*provider, output.Get(), 4, kUav);
    uint64_t sum = 0;
    for (size_t i = 0; i < out.size(); i += 4) sum += out[i] + out[i + 1];
    CHECK(sum > 0);
  }
  dlss.reset();  // GPU idle: RunOnGpu waits for the end
}

namespace {

// Texture finer than the render resolution (period 1.5 render pixels): a temporal upscaler
// reconstructs it only if the jitter it receives matches where the frame was sampled.
float Pattern(float out_x, float out_y) {
  constexpr float kTwoPi = 6.2831853f;
  return 0.5f + 0.5f * std::sin(kTwoPi * out_x / 3.0f) * std::sin(kTwoPi * out_y / 3.0f);
}

// Mean error (0..255) of the output with respect to the texture after `frames` frames, with the color sampled in the
// presenter's convention (projection shifted by +jitter pixels: the pixel center sees the point
// center - jitter) and the jitter passed to the upscaler multiplied by `sign`.
double ReconstructionError(const xe::ui::d3d12::D3D12Provider& provider, gpu::Upscaler& up, float sign) {
  constexpr uint32_t kRw = 320, kRh = 180, kOw = 640, kOh = 360;
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Device* device = provider.GetDevice();
  auto color = gow2_test::MakeTexture(device, kRw, kRh, DXGI_FORMAT_R8G8B8A8_UNORM, kRead);
  auto depth = gow2_test::MakeTexture(device, kRw, kRh, DXGI_FORMAT_R32_FLOAT, kRead);
  auto motion = gow2_test::MakeTexture(device, kRw, kRh, DXGI_FORMAT_R16G16_FLOAT, kRead);
  auto output = gow2_test::MakeTexture(device, kOw, kOh, DXGI_FORMAT_R8G8B8A8_UNORM, kUav,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const std::vector<float> half(kRw * kRh, 0.5f);
  gow2_test::Upload(provider, depth.Get(), 4, half.data(), kRead);
  const std::vector<uint32_t> zero_mv(kRw * kRh, 0);
  gow2_test::Upload(provider, motion.Get(), 4, zero_mv.data(), kRead);

  const uint32_t phases = up.JitterPhaseCount(kRw, kOw);
  std::vector<uint8_t> rgba(kRw * kRh * 4);
  for (uint32_t frame = 0; frame < 2 * phases; ++frame) {
    float jx, jy;
    up.JitterOffset(frame, phases, jx, jy);
    for (uint32_t y = 0; y < kRh; ++y)
      for (uint32_t x = 0; x < kRw; ++x) {
        const float v = Pattern((x + 0.5f - jx) * 2.0f, (y + 0.5f - jy) * 2.0f);
        uint8_t* p = &rgba[(y * kRw + x) * 4];
        p[0] = p[1] = p[2] = uint8_t(v * 255.0f + 0.5f), p[3] = 255;
      }
    gow2_test::Upload(provider, color.Get(), 4, rgba.data(), kRead);
    gow2_test::RunOnGpu(provider, [&](ID3D12GraphicsCommandList* l) {
      gpu::UpscaleFrame f{};
      f.cmd = l;
      f.color = color.Get(), f.depth = depth.Get(), f.motion = motion.Get(), f.output = output.Get();
      f.render_w = kRw, f.render_h = kRh, f.output_w = kOw, f.output_h = kOh;
      f.jitter_x = sign * jx, f.jitter_y = sign * jy;
      f.mv_scale_x = f.mv_scale_y = 1.0f;
      f.reset = frame == 0;
      f.sharpness = -1.0f;
      f.frame_ms = 16.6f;
      CHECK(up.Dispatch(f));
    });
  }
  const auto out = gow2_test::Readback(provider, output.Get(), 4, kUav);
  double err = 0;
  uint32_t n = 0;
  for (uint32_t y = 8; y < kOh - 8; ++y)  // borders excluded
    for (uint32_t x = 8; x < kOw - 8; ++x, ++n)
      err += std::abs(double(out[(y * kOw + x) * 4]) - Pattern(x + 0.5f, y + 0.5f) * 255.0);
  return err / n;
}

}  // namespace

TEST_CASE("dlss: jitter sign like FSR (reconstruction of a fine texture)", "[dlss]") {
  if (!std::filesystem::exists(kRealDir / "gow2_dlss.dll")) SKIP("gow2_dlss.dll not in the build (GOW2_DLSS_SDK_DIR)");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  if (provider->GetAdapterVendorID() != xe::ui::GraphicsProvider::GpuVendorID::kNvidia) SKIP("adapter not NVIDIA");
  ID3D12Device* device = provider->GetDevice();

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (auto fsr = gpu::CreateFsrUpscaler(device, 320, 180, 640, 360, std::filesystem::path(exe).parent_path())) {
    const double same = ReconstructionError(*provider, *fsr, 1.0f), flipped = ReconstructionError(*provider, *fsr, -1.0f);
    WARN("FSR error " << same << " / opposite " << flipped);
    CHECK(same < flipped);  // the generator follows the convention FSR already uses in game
  }
  auto dlss = gpu::CreateDlssUpscaler(device, 320, 180, 640, 360, kRealDir);
  REQUIRE(dlss);
  const double same = ReconstructionError(*provider, *dlss, 1.0f), flipped = ReconstructionError(*provider, *dlss, -1.0f);
  INFO("DLSS: error " << same << " with the presenter's sign, " << flipped << " with the opposite sign");
  CHECK(same < 0.8 * flipped);
  WARN("DLSS error " << same << " / opposite " << flipped);
}

// Sequence of Alt+Enter sizes (window, fullscreen, window): context recreated for the
// larger output, feature recreated in the same context for the smaller one, output released and recreated.
TEST_CASE("dlss: Alt+Enter sequence (context recreated, then smaller output)", "[dlss]") {
  if (!std::filesystem::exists(kRealDir / "gow2_dlss.dll")) SKIP("gow2_dlss.dll not in the build (GOW2_DLSS_SDK_DIR)");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  if (provider->GetAdapterVendorID() != xe::ui::GraphicsProvider::GpuVendorID::kNvidia) SKIP("adapter not NVIDIA");
  ID3D12Device* device = provider->GetDevice();
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto color = gow2_test::MakeTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, kRead);
  auto depth = gow2_test::MakeTexture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT, kRead);
  auto motion = gow2_test::MakeTexture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, kRead);
  auto mask = gow2_test::MakeTexture(device, 1280, 720, DXGI_FORMAT_R8_UNORM, kRead);
  // Like the presenter: the old output is released right before creating the new one (same pointer).
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  auto run = [&](gpu::Upscaler& up, uint32_t ow, uint32_t oh) {
    output.Reset();
    output = gow2_test::MakeTexture(device, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, kUav,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (uint32_t frame = 0; frame < 4; ++frame)
      gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
        gpu::UpscaleFrame f{};
        f.cmd = l;
        f.color = color.Get(), f.depth = depth.Get(), f.motion = motion.Get(), f.output = output.Get();
        f.reactive = mask.Get();
        f.render_w = 1280, f.render_h = 720, f.output_w = ow, f.output_h = oh;
        f.mv_scale_x = f.mv_scale_y = 1.0f;
        f.reset = frame == 0;
        f.sharpness = -1.0f;
        f.frame_ms = 16.6f;
        CHECK(up.Dispatch(f));
      });
  };
  auto first = gpu::CreateDlssUpscaler(device, 1280, 720, 1920, 1080, kRealDir);
  REQUIRE(first);
  run(*first, 1920, 1080);
  first.reset();
  auto second = gpu::CreateDlssUpscaler(device, 1280, 720, 2560, 1440, kRealDir);
  REQUIRE(second);
  run(*second, 2560, 1440);
  run(*second, 1920, 1080);
  CHECK(device->GetDeviceRemovedReason() == S_OK);

  // Like d3d12_backend: the probe stays alive while the presenter destroys and recreates its context (NGX stays open
  // for proxies like OptiScaler); the recreated context works and the probe is destroyed last.
  second.reset();
  auto probe = gpu::CreateDlssUpscaler(device, 1280, 720, 1280, 720, kRealDir);
  REQUIRE(probe);
  auto live = gpu::CreateDlssUpscaler(device, 1280, 720, 1920, 1080, kRealDir);
  REQUIRE(live);
  run(*live, 1920, 1080);
  live.reset();
  live = gpu::CreateDlssUpscaler(device, 1280, 720, 2560, 1440, kRealDir);
  REQUIRE(live);
  run(*live, 2560, 1440);
  live.reset();
  probe.reset();
  CHECK(device->GetDeviceRemovedReason() == S_OK);
}
