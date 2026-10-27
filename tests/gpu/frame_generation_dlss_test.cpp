// DLSS frame generation generator (runtime/gpu/frame_generation_dlss.cpp) with the fake bridge
// (tests/gpu/fake_dlss_bridge.cpp) in GOW2_FAKE_DLSS_DIR: generator ring, evaluate, timing thread that
// presents on the real swap chain, resize and fallback reasons. D3D12 debug layer on if installed.
// SKIP without a hardware D3D12 adapter.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "gpu/dlss_bridge/gow2_dlss_bridge.h"
#include "gpu/frame_generation.h"
#include "log.h"
#include "platform/platform.h"
#include "temp_dir.h"
#include "xenia/base/cvar.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

DECLARE_bool(d3d12_debug);

namespace {

using Microsoft::WRL::ComPtr;

const std::filesystem::path kFakeDir = GOW2_FAKE_DLSS_DIR;
constexpr uint32_t kW = 1280, kH = 720, kRW = 640, kRH = 360;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct ScopedEnv {
  const char* name;
  ScopedEnv(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
  ~ScopedEnv() { SetEnvironmentVariableA(name, nullptr); }
};

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

size_t Occurrences(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r;
  dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kRead, nullptr, IID_PPV_ARGS(&r));
  return r;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER x{};
  x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource = r;
  x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  x.Transition.StateBefore = a;
  x.Transition.StateAfter = b;
  cmd->ResourceBarrier(1, &x);
}

// Moving camera (like dlss_fg_math_test): non-identity clipToPrevClip.
gpu::UberPostConstants MovingCamera() {
  gpu::UberPostConstants k{};
  const float c2[4] = {0, 0, 1e-1f, -1e-4f};
  const float c13[4] = {0.98f, 0.05f, 0.01f, 0.02f};
  const float c14[4] = {-0.04f, 1.02f, 0.03f, -0.01f};
  const float c15[4] = {0.10f, -0.20f, 0.30f, 0.99f};
  const float c16[4] = {0.5f, -0.3f, 0.2f, 0.05f};
  for (int i = 0; i < 4; ++i) {
    k.c2[i] = c2[i], k.c13[i] = c13[i], k.c14[i] = c14[i], k.c15[i] = c15[i], k.c16[i] = c16[i];
  }
  return k;
}

// Device (with debug layer if installed), invisible window, request like Presenter::Initialize and a simulated
// presenter frame: CurrentBufferIndex, drawing (barriers), Frame, execution, Present.
struct Rig {
  std::unique_ptr<xe::ui::d3d12::D3D12Provider> provider;
  plat::Window* window = nullptr;
  gpu::SwapChainRequest req{};
  ComPtr<ID3D12InfoQueue> info;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> cmd;
  ComPtr<ID3D12Fence> fence;
  uint64_t fv = 0, frame_id = 0;
  bool prev_inputs = false;
  ComPtr<ID3D12Resource> depth, motion, hudless;
  gpu::UberPostConstants k = MovingCamera();

  bool Init() {
    cvars::d3d12_debug = true;
    provider = xe::ui::d3d12::D3D12Provider::Create();
    cvars::d3d12_debug = false;
    if (!provider) return false;
    window = plat::CreateGameWindow("gow2 frame generation dlss test", {kW, kH}, false, [] {});
    if (!window) return false;
    ID3D12Device* dev = provider->GetDevice();
    dev->QueryInterface(IID_PPV_ARGS(&info));  // nullptr without the debug layer installed
    req.device = dev;
    req.factory = provider->GetDXGIFactory();
    req.queue = provider->GetDirectQueue();
    req.hwnd = static_cast<HWND>(plat::NativeHandle(window));
    req.desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    req.desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    req.desc.SampleDesc.Count = 1;
    req.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    req.desc.BufferCount = 2;  // the presenter's kBufferCount
    req.desc.Scaling = DXGI_SCALING_STRETCH;
    req.desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    req.desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
      return false;
    cmd->Close();
    depth = MakeTex(dev, DXGI_FORMAT_R32_FLOAT, kRW, kRH);
    motion = MakeTex(dev, DXGI_FORMAT_R16G16_FLOAT, kRW, kRH);
    hudless = MakeTex(dev, DXGI_FORMAT_R10G10B10A2_UNORM, kW, kH);
    return depth && motion && hudless;
  }
  ~Rig() {
    if (window) plat::DestroyGameWindow(window);
  }

  ComPtr<ID3D12DescriptorHeap> rtv;
  const float* clear = nullptr;  // color of the real frame, if set

  void Frame(gpu::FrameGenerator& fg, IDXGISwapChain4* sc, bool inputs, uint32_t w = kW, uint32_t h = kH) {
    ++frame_id;
    const UINT i = fg.CurrentBufferIndex();
    ComPtr<ID3D12Resource> bb;
    REQUIRE(SUCCEEDED(fg.GetBuffer(i, &bb)));
    REQUIRE(bb->GetDesc().Width == w);
    alloc->Reset();
    cmd->Reset(alloc.Get(), nullptr);
    Barrier(cmd.Get(), bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (clear) {
      if (!rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        REQUIRE(SUCCEEDED(provider->GetDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv))));
      }
      const D3D12_CPU_DESCRIPTOR_HANDLE h = rtv->GetCPUDescriptorHandleForHeapStart();
      provider->GetDevice()->CreateRenderTargetView(bb.Get(), nullptr, h);
      cmd->ClearRenderTargetView(h, clear, 0, nullptr);
    }
    if (inputs) {
      gpu::FrameGenInputs in{cmd.Get(), depth.Get(), motion.Get(), hudless.Get(), kRW, kRH, 0.25f, -0.25f, 0.0f, &k};
      fg.Frame(sc, w, h, gpu::FrameGenFrame{frame_id, true, !prev_inputs, 16.7f}, &in);
    } else {
      fg.Frame(sc, w, h, gpu::FrameGenFrame{frame_id, false, false, 16.7f}, nullptr);
    }
    prev_inputs = inputs;
    Barrier(cmd.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    REQUIRE(SUCCEEDED(cmd->Close()));
    ID3D12CommandList* lists[] = {cmd.Get()};
    req.queue->ExecuteCommandLists(1, lists);
    req.queue->Signal(fence.Get(), ++fv);
    if (fence->GetCompletedValue() < fv) fence->SetEventOnCompletion(fv, nullptr);  // allocator free
    CHECK(fg.Present(0, 0) == S_OK);
  }

  // Debug layer ERROR and CORRUPTION messages so far.
  std::vector<std::string> DebugErrors() const {
    std::vector<std::string> out;
    if (!info) return out;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      info->GetMessage(i, nullptr, &size);
      std::vector<char> buf(size);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
      if (FAILED(info->GetMessage(i, m, &size))) continue;
      if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        out.emplace_back(m->pDescription, m->DescriptionByteLength);
    }
    return out;
  }
};

UINT LastPresentCount(IDXGISwapChain4* sc) {
  UINT n = 0;
  sc->GetLastPresentCount(&n);
  return n;
}

template <typename F>
F Export(const char* name) {
  return reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(L"gow2_dlss.dll"), name));
}

}  // namespace

TEST_CASE("frame generation DLSS: bridge missing", "[frame_generation_dlss]") {
  gow2_test::TempDir dir("frame_generation_dlss");
  LogInit(dir.path / "log.txt");
  ComPtr<IDXGISwapChain4> out;
  CHECK_FALSE(gpu::CreateDlssFrameGenerator(gpu::SwapChainRequest{}, out, dir.path));
  CHECK_FALSE(out);
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  CHECK(log.find("[fg] DLSS frame generation not available (") != std::string::npos);
  CHECK(log.find("): normal swap chain") != std::string::npos);
}

TEST_CASE("frame generation DLSS: fallback reasons", "[frame_generation_dlss]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  Rig rig;
  REQUIRE(rig.Init());
  struct Case {
    const char *env, *value, *reason;
  };
  const Case cases[] = {
      {"GOW2_FAKE_DLSS_ABI", "2", "(ABI 2 bridge, 3 required)"},
      {"GOW2_FAKE_DLSS_FG", "0", "(GPU or driver without DLSS frame generation)"},
      {"GOW2_FAKE_DLSS_FG_CREATE_FAIL", "1", "(feature not created)"},
  };
  for (const Case& c : cases) {
    gow2_test::TempDir dir("frame_generation_dlss");
    LogInit(dir.path / "log.txt");
    {
      ScopedEnv env(c.env, c.value);
      ComPtr<IDXGISwapChain4> out;
      CHECK_FALSE(gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir));
      CHECK_FALSE(out);
    }
    LogInit({});
    const std::string log = ReadText(dir.path / "log.txt");
    INFO(log);
    CHECK(Occurrences(log, "[fg] DLSS frame generation not available " + std::string(c.reason) +
                               ": normal swap chain") == 1);
  }
  {  // driver needs updating: with the minimum
    gow2_test::TempDir dir("frame_generation_dlss");
    LogInit(dir.path / "log.txt");
    {
      ScopedEnv off("GOW2_FAKE_DLSS_FG", "0"), driver("GOW2_FAKE_DLSS_FG_NEEDS_DRIVER", "1");
      ComPtr<IDXGISwapChain4> out;
      CHECK_FALSE(gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir));
    }
    LogInit({});
    CHECK(ReadText(dir.path / "log.txt").find("(driver 555.85 or later required)") != std::string::npos);
  }
  // After a fallback the window still accepts a normal swap chain (the presenter's).
  ComPtr<IDXGISwapChain1> sc;
  CHECK(SUCCEEDED(rig.req.factory->CreateSwapChainForHwnd(rig.req.queue, rig.req.hwnd, &rig.req.desc, nullptr,
                                                          nullptr, &sc)));
}

TEST_CASE("frame generation DLSS: one generated and one real per frame with inputs", "[frame_generation_dlss]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  Rig rig;
  REQUIRE(rig.Init());
  gow2_test::TempDir dir("frame_generation_dlss");
  LogInit(dir.path / "log.txt");
  {
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir);
    REQUIRE(fg);
    REQUIRE(out);
    DXGI_SWAP_CHAIN_DESC1 d{};
    out->GetDesc1(&d);
    CHECK(d.BufferCount == 3);
    const auto evaluations = Export<uint32_t (*)()>("FakeDlssFgEvaluations");
    const auto last = Export<const Gow2DlssFgFrame* (*)()>("FakeDlssLastFgFrame");
    REQUIRE(evaluations != nullptr);
    REQUIRE(last != nullptr);
    const uint32_t e0 = evaluations();
    const UINT p0 = LastPresentCount(out.Get());

    rig.Frame(*fg, out.Get(), true);
    const Gow2DlssFgFrame first = *last();
    CHECK(first.reset == 1);
    ComPtr<ID3D12Resource> b0;
    REQUIRE(SUCCEEDED(fg->GetBuffer(0, &b0)));
    CHECK(first.backbuffer == b0.Get());
    CHECK(first.output);
    CHECK(first.output != first.backbuffer);
    CHECK(first.depth == rig.depth.Get());
    CHECK(first.motion == rig.motion.Get());
    CHECK(first.hudless == rig.hudless.Get());
    CHECK(first.render_w == kRW);
    CHECK(first.render_h == kRH);
    CHECK(first.jitter_x == 0.25f);
    CHECK(first.jitter_y == -0.25f);
    CHECK(first.mv_scale_x == 1.0f);
    CHECK(first.mv_scale_y == 1.0f);
    CHECK(first.near_z == 10.0f);
    CHECK(first.far_z == 1e6f);
    CHECK(first.fov_y == 1.0f);
    CHECK(first.aspect == Catch::Approx(float(kRW) / kRH));
    CHECK(first.depth_inverted == 1);
    const auto identity = [](const float* m) {
      for (int i = 0; i < 16; ++i)
        if (m[i] != (i % 5 == 0 ? 1.0f : 0.0f)) return false;
      return true;
    };
    CHECK_FALSE(identity(first.clip_to_prev_clip));
    CHECK_FALSE(identity(first.prev_clip_to_clip));
    CHECK_FALSE(identity(first.view_to_clip));
    CHECK_FALSE(identity(first.clip_to_view));
    float prod[16] = {};  // clip_to_prev_clip * prev_clip_to_clip = identity
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        for (int j = 0; j < 4; ++j)
          prod[r * 4 + c] += first.clip_to_prev_clip[r * 4 + j] * first.prev_clip_to_clip[j * 4 + c];
    for (int i = 0; i < 16; ++i) CHECK(prod[i] == Catch::Approx(i % 5 == 0 ? 1.0f : 0.0f).margin(1e-3));

    for (int i = 1; i < 10; ++i) rig.Frame(*fg, out.Get(), true);
    CHECK(last()->reset == 0);
    fg->WaitForPresents();
    CHECK(evaluations() - e0 == 10);
    CHECK(LastPresentCount(out.Get()) - p0 == 19);  // the first (reset) real only

    // Without inputs: only the real one.
    for (int i = 0; i < 5; ++i) rig.Frame(*fg, out.Get(), false);
    fg->WaitForPresents();
    CHECK(evaluations() - e0 == 10);
    CHECK(LastPresentCount(out.Get()) - p0 == 24);
    CHECK(rig.provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
    fg->Shutdown(out);
    CHECK_FALSE(out);
  }
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  INFO(log);
  CHECK(log.find("[fg] fake DLSS FG (1280 x 720)") != std::string::npos);
  CHECK(log.find("[fg] closing: real 15, generated 9") != std::string::npos);
  CHECK(rig.DebugErrors().empty());
  if (!rig.info) WARN("D3D12 debug layer not installed: errors not checked");
}

TEST_CASE("frame generation DLSS: evaluate fails only once, then only real frames", "[frame_generation_dlss]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  Rig rig;
  REQUIRE(rig.Init());
  gow2_test::TempDir dir("frame_generation_dlss");
  LogInit(dir.path / "log.txt");
  {
    ScopedEnv fail("GOW2_FAKE_DLSS_FG_EVAL_FAIL", "1");
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir);
    REQUIRE(fg);
    const auto evaluations = Export<uint32_t (*)()>("FakeDlssFgEvaluations");
    REQUIRE(evaluations != nullptr);
    const uint32_t e0 = evaluations();
    const UINT p0 = LastPresentCount(out.Get());
    for (int i = 0; i < 8; ++i) rig.Frame(*fg, out.Get(), true);
    fg->WaitForPresents();
    CHECK(evaluations() - e0 == 1);
    CHECK(LastPresentCount(out.Get()) - p0 == 8);  // real only
    CHECK(rig.provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
    fg->Shutdown(out);
  }
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  INFO(log);
  CHECK(Occurrences(log, "[fg] DLSS FG evaluate failed") == 1);
  CHECK(rig.DebugErrors().empty());
}

TEST_CASE("frame generation DLSS: hot resize and shutdown", "[frame_generation_dlss]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  Rig rig;
  REQUIRE(rig.Init());
  gow2_test::TempDir dir("frame_generation_dlss");
  LogInit(dir.path / "log.txt");
  {
    ScopedEnv desc("GOW2_FAKE_DLSS_FG_DESC", "0");  // NGX says nothing: fallback line
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir);
    REQUIRE(fg);
    const UINT p0 = LastPresentCount(out.Get());
    for (int i = 0; i < 3; ++i) rig.Frame(*fg, out.Get(), true);
    // Like Presenter::ResizeIfNeeded: GPU idle, WaitForPresents, ResizeBuffers, buffers again.
    fg->WaitForPresents();
    DXGI_SWAP_CHAIN_DESC1 d{};
    out->GetDesc1(&d);
    REQUIRE(SUCCEEDED(out->ResizeBuffers(0, 960, 540, DXGI_FORMAT_UNKNOWN, d.Flags)));
    out->GetDesc1(&d);
    CHECK(d.BufferCount == 3);
    for (UINT i = 0; i < 2; ++i) {
      ComPtr<ID3D12Resource> b;
      REQUIRE(SUCCEEDED(fg->GetBuffer(i, &b)));
      CHECK(b->GetDesc().Width == 960);
      CHECK(b->GetDesc().Height == 540);
    }
    for (int i = 0; i < 5; ++i) rig.Frame(*fg, out.Get(), true, 960, 540);
    fg->WaitForPresents();
    CHECK(LastPresentCount(out.Get()) - p0 == 14);  // reset at the first and after the feature is recreated
    // Hot shutdown: from here Frame without inputs, nothing generated.
    for (int i = 0; i < 5; ++i) rig.Frame(*fg, out.Get(), false, 960, 540);
    fg->WaitForPresents();
    CHECK(LastPresentCount(out.Get()) - p0 == 19);
    CHECK(rig.provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
    fg->Shutdown(out);
  }
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  INFO(log);
  CHECK(log.find("[fg] DLSS frame generation (1280 x 720)") != std::string::npos);
  CHECK(log.find("[fg] DLSS frame generation (960 x 540)") != std::string::npos);
  CHECK(log.find("[fg] closing: real 13, generated 6") != std::string::npos);
  const auto errors = rig.DebugErrors();
  for (const auto& e : errors) UNSCOPED_INFO(e);
  CHECK(errors.empty());
}
TEST_CASE("frame generation DLSS: the Presents do not wait for game work queued after the frame",
          "[frame_generation_dlss]") {
  // Cause of the 1/3 pace: the generated frame's copy and Present on the game queue started behind
  // the next frame's work (~2 ms), hence a vblank later at 165 Hz. Here the game queue stays stalled after the frame
  // until the test unblocks it: the frame's real one must still reach the real swap chain's back buffer.
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  Rig rig;
  REQUIRE(rig.Init());
  ID3D12Device* dev = rig.provider->GetDevice();
  gow2_test::TempDir dir("frame_generation_dlss");
  LogInit(dir.path / "log.txt");
  // Reading a back buffer pixel from a test queue (implicit promotion from COMMON, no barriers).
  ComPtr<ID3D12CommandQueue> q;
  ComPtr<ID3D12CommandAllocator> a;
  ComPtr<ID3D12GraphicsCommandList> c;
  ComPtr<ID3D12Fence> blocker, done;
  ComPtr<ID3D12Resource> rb;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 3 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))));
  REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))));
  REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&c))));
  c->Close();
  REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker))));
  REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done))));
  REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&rb))));
  uint64_t dv = 0;
  {
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateDlssFrameGenerator(rig.req, out, kFakeDir);
    REQUIRE(fg);
    const auto Shown = [&](uint32_t want) {  // pixel (0,0) of a back buffer equals `want`
      a->Reset();
      c->Reset(a.Get(), nullptr);
      for (UINT i = 0; i < 3; ++i) {
        ComPtr<ID3D12Resource> b;
        REQUIRE(SUCCEEDED(out->GetBuffer(i, IID_PPV_ARGS(&b))));
        D3D12_TEXTURE_COPY_LOCATION from{b.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, to{rb.Get()};
        from.SubresourceIndex = 0;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = i * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        const D3D12_BOX px{0, 0, 0, 1, 1, 1};
        c->CopyTextureRegion(&to, 0, 0, 0, &from, &px);
      }
      REQUIRE(SUCCEEDED(c->Close()));
      ID3D12CommandList* lists[] = {c.Get()};
      q->ExecuteCommandLists(1, lists);
      q->Signal(done.Get(), ++dv);
      done->SetEventOnCompletion(dv, nullptr);
      void* p = nullptr;
      REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &p)));
      bool found = false;
      for (UINT i = 0; i < 3; ++i)
        found = found ||
                *reinterpret_cast<const uint32_t*>(static_cast<const char*>(p) +
                                                   i * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) == want;
      rb->Unmap(0, nullptr);
      return found;
    };
    for (int i = 0; i < 4; ++i) rig.Frame(*fg, out.Get(), true);
    fg->WaitForPresents();
    constexpr float kRed[4] = {1, 0, 0, 1};
    constexpr uint32_t kRedPixel = 0xC00003FFu;  // R10G10B10A2: R = 1023, A = 3
    const UINT p0 = LastPresentCount(out.Get());
    rig.clear = kRed;
    rig.Frame(*fg, out.Get(), true);
    rig.req.queue->Wait(blocker.Get(), 1);  // game work after the frame, still in progress
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (LastPresentCount(out.Get()) < p0 + 2 && std::chrono::steady_clock::now() < deadline) Sleep(1);
    CHECK(LastPresentCount(out.Get()) == p0 + 2);
    Sleep(200);  // copies finished on the GPU; a single read, with the generator idle
    {
      const auto errors = rig.DebugErrors();  // of the generator, before the test's read
      for (const auto& e : errors) UNSCOPED_INFO(e);
      CHECK(errors.empty());
    }
    const bool shown = Shown(kRedPixel);
    // The test's read does not synchronize with the generator's queues: the debug layer reports it, it does not count.
    if (rig.info) rig.info->ClearStoredMessages();
    blocker->Signal(1);
    CHECK(shown);
    fg->WaitForPresents();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    fg->Shutdown(out);
  }
  LogInit({});
  const auto errors = rig.DebugErrors();
  for (const auto& e : errors) UNSCOPED_INFO(e);
  CHECK(errors.empty());
}
