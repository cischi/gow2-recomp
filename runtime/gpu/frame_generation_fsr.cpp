// See frame_generation.h. FSR 3 frame generation through the FidelityFX API (SDK v2.3.0, thirdparty/fidelityfx):
// the amd_fidelityfx_loader_dx12.dll loader is loaded at runtime, as for the FSR upscaler, with its own HMODULE.
#ifndef _WINDOWS
#define _WINDOWS  // ffx_api_loader.h declares ffxLoadFunctions only with _WINDOWS or PLATFORM_WINDOWS
#endif
#include "gpu/frame_generation.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "dx12/ffx_api_dx12.h"
#include "dx12/ffx_api_framegeneration_dx12.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_framegeneration.h"
#include "gpu/frame_pacer.h"
#include "log.h"

namespace gpu {

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr wchar_t kLoaderDll[] = L"amd_fidelityfx_loader_dx12.dll";

std::string Narrow(const wchar_t* w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

void OnMessage(uint32_t type, const wchar_t* message) {
  LogLine(std::string("[fg] ffx ") + (type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "warning: ") + Narrow(message));
}

void Unavailable(const std::string& reason) {
  LogLine("[fg] frame generation not available (" + reason + "): normal swap chain");
}

D3D12_RESOURCE_STATES ToD3d(uint32_t s) {
  switch (s) {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
      return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;  // COMMON and PRESENT
  }
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  if (a == b) return;
  D3D12_RESOURCE_BARRIER x{};
  x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource = r;
  x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  x.Transition.StateBefore = a;
  x.Transition.StateAfter = b;
  cmd->ResourceBarrier(1, &x);
}

long long MsSince(Clock::time_point t0) {
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

class FsrFrameGenerator final : public FrameGenerator {
 public:
  FsrFrameGenerator(HMODULE dll, const ffxFunctions& ffx, ffxContext sc_ctx, ID3D12Device* device,
                    IDXGISwapChain4* proxy)
      : dll_(dll), ffx_(ffx), sc_ctx_(sc_ctx), device_(device), proxy_(proxy) {}
  ~FsrFrameGenerator() override { FreeLibrary(dll_); }

  // Forwarding to the proxy, owned by the presenter until Shutdown.
  HRESULT GetBuffer(UINT i, ID3D12Resource** out) override { return proxy_->GetBuffer(i, IID_PPV_ARGS(out)); }
  UINT CurrentBufferIndex() override { return proxy_->GetCurrentBackBufferIndex(); }
  HRESULT Present(UINT sync_interval, UINT flags) override {
    const HRESULT hr = proxy_->Present(sync_interval, flags);
    SampleDisplayed();
    return hr;
  }

  void Frame(IDXGISwapChain4* swap_chain, uint32_t w, uint32_t h, const FrameGenFrame& f,
             const FrameGenInputs* in) override {
    SampleDisplayed();
    last_frame_id_ = f.frame_id;
    if (in) hdr_peak_.store(in->hdr_peak_nits);
    if (in && !EnsureFg(swap_chain, f.frame_id, w, h, in->render_w, in->render_h, in->hdr_peak_nits > 0)) {
      if (enabled_) LogLine("[fg] generation off at frame " + std::to_string(f.frame_id));
      enabled_ = false;
      return;
    }
    if (!fg_ctx_) return;
    const bool enabled = in != nullptr;
    if (enabled != enabled_) {
      LogLine(std::string("[fg] generation ") + (enabled ? "on" : "off") + " at frame " +
              std::to_string(f.frame_id));
      enabled_ = enabled;
    }
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = swap_chain;
    c.presentCallback = [](ffxCallbackDescFrameGenerationPresent* d, void* u) {
      return static_cast<FsrFrameGenerator*>(u)->OnPresent(d);
    };
    c.presentCallbackUserContext = this;
    c.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* d, void* u) -> ffxReturnCode_t {
      auto* s = static_cast<FsrFrameGenerator*>(u);
      if (const float peak = s->hdr_peak_.load(); peak > 0) {  // back buffer HDR10
        d->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        d->minMaxLuminance[0] = 0.0f;
        d->minMaxLuminance[1] = peak;
      }
      return s->ffx_.Dispatch(&s->fg_ctx_, &d->header);
    };
    c.frameGenerationCallbackUserContext = this;
    c.frameGenerationEnabled = enabled;
    c.allowAsyncWorkloads = false;
    if (in) c.HUDLessColor = ffxApiGetResourceDX12(in->hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    c.flags = 0;
    c.onlyPresentGenerated = false;
    c.generationRect = {0, 0, int32_t(display_w_), int32_t(display_h_)};
    c.frameID = f.frame_id;
    Configure(c);
    if (!in) return;

    ffxDispatchDescFrameGenerationPrepareV2 d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = f.frame_id;
    d.flags = 0;
    d.commandList = in->cmd;
    d.renderSize = {in->render_w, in->render_h};
    d.jitterOffset = {in->jitter_x, in->jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};
    d.frameTimeDelta = f.frame_ms;
    d.reset = f.reset;
    // shortcut: fake camera (axes, FOV 1 rad, near 10, far 1e6, 2 cm/unit) like the FSR upscaler; the real camera
    // from the game's constants if the visual check shows defects in rotation or in disocclusions.
    d.cameraNear = 10.0f;
    d.cameraFar = 1e6f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 0.02f;
    const float up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
    for (int i = 0; i < 3; ++i) {
      d.cameraPosition[i] = 0;
      d.cameraUp[i] = up[i];
      d.cameraRight[i] = right[i];
      d.cameraForward[i] = fwd[i];
    }
    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    d.depth = ffxApiGetResourceDX12(in->depth, kRead);
    d.motionVectors = ffxApiGetResourceDX12(in->motion, kRead);
    const ffxReturnCode_t rc = ffx_.Dispatch(&fg_ctx_, &d.header);
    if (rc != FFX_API_RETURN_OK && !prepare_failed_) {
      LogLine("[fg] prepare failed (code " + std::to_string(rc) + ")");
      prepare_failed_ = true;
    }
  }

  void WaitForPresents() override {
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    ffx_.Dispatch(&sc_ctx_, &wait.header);
  }

  void Shutdown(ComPtr<IDXGISwapChain4>& swap_chain) override {
    if (fg_ctx_) DisableFg(swap_chain.Get(), last_frame_id_ + 1);
    WaitForPresents();
    if (fg_ctx_) ffx_.DestroyContext(&fg_ctx_, nullptr);
    fg_ctx_ = nullptr;
    swap_chain.Reset();
    proxy_ = nullptr;
    ffx_.DestroyContext(&sc_ctx_, nullptr);
    sc_ctx_ = nullptr;
    LogLine("[fg] closing: real " + std::to_string(real_.load()) + ", generated " + std::to_string(generated_.load()));
  }

 private:
  // Label (generated or real) of the last PresentCount + 1: from the proxy's present callback, once per
  // Present of the real swap chain.
  // shortcut: GetLastPresentCount()+1 is the Present that follows the callback (true as long as only the proxy's
  // present thread presents the real swap chain), an explicit count from the proxy if other code presents.
  void Label(bool generated) {
    UINT last = 0;
    if (FAILED(proxy_->GetLastPresentCount(&last))) return;
    std::lock_guard g(displayed_mutex_);
    labels_[(last + 1) % labels_.size()] = {last + 1, generated};
  }
  // Last frame on screen, if new and labeled; missed counts only break the P triples. Samples from
  // three points: the G and R callbacks arrive one after the other before G goes on screen, the Present thread
  // alone sees one new count per game frame.
  void SampleDisplayed() {
    DXGI_FRAME_STATISTICS s{};
    if (FAILED(proxy_->GetFrameStatistics(&s))) return;  // outside the mutex: no inversion with the proxy's callback
    std::lock_guard g(displayed_mutex_);
    if (s.PresentCount == last_count_) return;
    last_count_ = s.PresentCount;
    const CountLabel& l = labels_[s.PresentCount % labels_.size()];
    if (l.count == s.PresentCount) displayed_.push_back({l.generated, s.PresentCount, s.SyncQPCTime.QuadPart});
  }

  void Configure(const ffxConfigureDescFrameGeneration& c) {
    const ffxReturnCode_t rc = ffx_.Configure(&fg_ctx_, &c.header);
    if (rc != FFX_API_RETURN_OK && !configure_failed_) {
      LogLine("[fg] configure failed (code " + std::to_string(rc) + ")");
      configure_failed_ = true;
    }
  }

  // Before destroying the FG context (AMD's sequence): generation off with the real swap chain, callback and
  // HUDLessColor empty, so the proxy holds no pointers to the context or the HUD-less.
  void DisableFg(IDXGISwapChain4* swap_chain, uint64_t frame_id) {
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = swap_chain;
    c.frameGenerationEnabled = false;
    c.frameID = frame_id;
    Configure(c);
  }

  // FG context for output w x h, render up to rw x rh and HDR or SDR; recreated (after DisableFg and WaitForPresents)
  // if they change.
  bool EnsureFg(IDXGISwapChain4* swap_chain, uint64_t frame_id, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh,
                bool hdr) {
    const bool same = w == display_w_ && h == display_h_ && rw <= max_rw_ && rh <= max_rh_ && hdr == hdr_;
    if (fg_ctx_ && same) return true;
    // Creation failed: no new attempt until output, maximum render and HDR change.
    if (!fg_ctx_ && failed_ && same) return false;
    if (fg_ctx_) {
      DisableFg(swap_chain, frame_id);
      WaitForPresents();
      ffx_.DestroyContext(&fg_ctx_, nullptr);
      fg_ctx_ = nullptr;
    }
    display_w_ = w, display_h_ = h, max_rw_ = rw, max_rh_ = rh, hdr_ = hdr;
    ffxCreateContextDescFrameGeneration c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    c.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (hdr) c.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    c.displaySize = {w, h};
    c.maxRenderSize = {rw, rh};
    c.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT_R10G10B10A2_UNORM);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device_;
    ffxCreateContextDescFrameGenerationVersion v{};
    v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    v.version = FFX_FRAMEGENERATION_VERSION;
    c.header.pNext = &backend.header;
    backend.header.pNext = &v.header;
    const auto t0 = Clock::now();
    const ffxReturnCode_t rc = ffx_.CreateContext(&fg_ctx_, &c.header, nullptr);
    failed_ = rc != FFX_API_RETURN_OK;
    if (failed_) {
      LogLine("[fg] FG context not created (code " + std::to_string(rc) + ")");
      fg_ctx_ = nullptr;
      return false;
    }
    const long long ms = MsSince(t0);
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    ffx_.Query(&fg_ctx_, &pv.header);
    FfxApiEffectMemoryUsage fg_mem{}, sc_mem{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    q.gpuMemoryUsageFrameGeneration = &fg_mem;
    ffx_.Query(&fg_ctx_, &q.header);
    ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 qs{};
    qs.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12;
    qs.gpuMemoryUsageFrameGenerationSwapchain = &sc_mem;
    ffx_.Query(&sc_ctx_, &qs.header);
    char line[256];
    std::snprintf(line, sizeof line,
                  "[fg] FG context %s: output %ux%u, max render %ux%u, %lld ms; FG memory %.1f MB, swap chain %.1f MB%s",
                  pv.versionName ? pv.versionName : "?", w, h, rw, rh, ms, fg_mem.totalUsageInBytes / 1048576.0,
                  sc_mem.totalUsageInBytes / 1048576.0, hdr ? ", HDR" : "");
    LogLine(line);
    return true;
  }

  // Proxy present thread: the frame (real or generated) must be copied into the real swap chain's buffer.
  ffxReturnCode_t OnPresent(ffxCallbackDescFrameGenerationPresent* d) {
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(d->commandList);
    auto* src = static_cast<ID3D12Resource*>(d->currentBackBuffer.resource);
    auto* dst = static_cast<ID3D12Resource*>(d->outputSwapChainBuffer.resource);
    const D3D12_RESOURCE_STATES ss = ToD3d(d->currentBackBuffer.state), ds = ToD3d(d->outputSwapChainBuffer.state);
    Barrier(cmd, src, ss, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, dst, ds, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION from{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    to.SubresourceIndex = from.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, ss);
    Barrier(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, ds);

    Label(d->isGeneratedFrame);
    SampleDisplayed();
    (d->isGeneratedFrame ? generated_ : real_).fetch_add(1);
    ++(d->isGeneratedFrame ? window_generated_ : window_real_);
    const auto now = Clock::now();
    if (window_start_ == Clock::time_point{}) window_start_ = now;
    if (now - window_start_ >= std::chrono::seconds(10)) {
      std::string line =
          "[fg] last 10 s: real " + std::to_string(window_real_) + ", generated " + std::to_string(window_generated_);
      {
        std::lock_guard g(displayed_mutex_);
        if (const std::optional<float> p = MedianHalfIntervalPercent(displayed_)) {  // omitted without statistics
          char tail[64];
          std::snprintf(tail, sizeof tail, ", half-interval median %.1f%%", *p);
          line += tail;
        }
        displayed_.clear();
      }
      LogLine(line);
      window_start_ = now;
      window_real_ = window_generated_ = 0;
    }
    return FFX_API_RETURN_OK;
  }

  HMODULE dll_;
  ffxFunctions ffx_;
  ffxContext sc_ctx_;
  ffxContext fg_ctx_ = nullptr;
  ID3D12Device* device_;
  IDXGISwapChain4* proxy_;
  // Present thread.
  uint32_t display_w_ = 0, display_h_ = 0, max_rw_ = 0, max_rh_ = 0;
  bool hdr_ = false;
  bool failed_ = false;  // last FG context creation failed with these dimensions
  uint64_t last_frame_id_ = 0;
  bool enabled_ = false, configure_failed_ = false, prepare_failed_ = false;
  // Proxy present thread (totals read after WaitForPresents).
  std::atomic<uint64_t> real_{0}, generated_{0};
  std::atomic<float> hdr_peak_{0};  // written in Frame, read by the generation callback
  uint64_t window_real_ = 0, window_generated_ = 0;
  Clock::time_point window_start_{};
  // Measurement of P: Present thread and proxy present thread.
  std::mutex displayed_mutex_;
  struct CountLabel {
    uint32_t count;  // PresentCount of the real swap chain
    bool generated;
  };
  std::array<CountLabel, 64> labels_{};  // indexed by PresentCount % 64: more than the frames queued between Present and scanout
  uint32_t last_count_ = 0;              // PresentCount of the last sample
  std::vector<DisplayedFrame> displayed_;  // of the 10 s window
};

}  // namespace

std::unique_ptr<FrameGenerator> CreateFsrFrameGenerator(const SwapChainRequest& req, ComPtr<IDXGISwapChain4>& out,
                                                        const std::filesystem::path& dll_dir) {
  out.Reset();
  const std::filesystem::path path = dll_dir / kLoaderDll;
  HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return Unavailable("loader missing"), nullptr;
  ffxFunctions ffx{};
  ffxLoadFunctions(&ffx, dll);
  if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Configure || !ffx.Dispatch || !ffx.Query) {
    Unavailable("loader functions missing");
    FreeLibrary(dll);
    return nullptr;
  }
  ffxConfigureDescGlobalDebug1 dbg{};
  dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
  dbg.fpMessage = OnMessage;
  dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
  ffx.Configure(nullptr, &dbg.header);

  DXGI_SWAP_CHAIN_DESC1 desc = req.desc;
  IDXGISwapChain4* sc = nullptr;
  ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d{};
  d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
  d.swapchain = &sc;
  d.hwnd = req.hwnd;
  d.desc = &desc;
  d.fullscreenDesc = nullptr;
  d.dxgiFactory = req.factory;
  d.gameQueue = req.queue;
  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 v{};
  v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
  v.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  d.header.pNext = &v.header;
  const auto t0 = Clock::now();
  ffxContext sc_ctx = nullptr;
  const ffxReturnCode_t rc = ffx.CreateContext(&sc_ctx, &d.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !sc) {
    if (sc_ctx) ffx.DestroyContext(&sc_ctx, nullptr);
    Unavailable("proxy swap chain not created, code " + std::to_string(rc));
    FreeLibrary(dll);
    return nullptr;
  }
  out.Attach(sc);
  ffxQueryGetProviderVersion pv{};
  pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx.Query(&sc_ctx, &pv.header);
  LogLine(std::string("[fg] swap chain proxy ") + (pv.versionName ? pv.versionName : "?") + " created in " +
          std::to_string(MsSince(t0)) + " ms");
  return std::make_unique<FsrFrameGenerator>(dll, ffx, sc_ctx, req.device, sc);
}

}  // namespace gpu
