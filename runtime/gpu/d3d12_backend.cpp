// See d3d12_backend.h.
#include "gpu/d3d12_backend.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "game_patches/game_patches.h"
#include "gpu/frame_generation.h"
#include "gpu/presenter.h"
#include "launcher/launcher.h"
#include "launcher/launcher_model.h"
#include "gpu/shader_overrides.h"
#include "gpu/xenia_backend.h"
#include "guest_memory.h"
#include "kernel/stats.h"
#include "log.h"
#include "platform/platform.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/gpu/d3d12/pipeline_cache.h"
#include "xenia/memory.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

// Defined in Xenia's code (d3d12_provider.cc, d3d12_render_target_cache.cc).
DECLARE_int32(d3d12_adapter);
DECLARE_int32(anisotropic_override);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_string(render_target_path_d3d12);

DEFINE_string(gow2_upscaler_debug, "none",
              "Upscaler debug: none, ffx_debug_view (FSR debug view).",
              "GPU");

namespace gpu {

namespace {

// Settings changed hot by the F1 overlay and read by the command processor thread.
struct LiveGpuSettings {
  std::atomic<uint32_t> antialiasing;  // SwapPostEffect: 0 with the upscaler active (FXAA ignored)
  std::atomic<bool> no_motion_blur;    // [patches] disable_motion_blur
};

// Xenia's D3D12CommandProcessor with gow2's presentation: in Xenia the
// presenter shows the guest output from the UI thread; here the
// command processor thread presents on every XE_SWAP.
class Gow2D3D12CommandProcessor final : public xe::gpu::d3d12::D3D12CommandProcessor {
 public:
  Gow2D3D12CommandProcessor(xe::gpu::d3d12::D3D12GraphicsSystem* graphics_system, Presenter* presenter,
                            std::shared_ptr<LiveGpuSettings> live)
      : D3D12CommandProcessor(graphics_system, graphics_system->kernel_state()),
        presenter_(presenter),
        live_(std::move(live)),
        antialiasing_(live_->antialiasing) {
    SetDesiredSwapPostEffect(SwapPostEffect(antialiasing_));
  }

 protected:
  // The base updates the guest output (RefreshGuestOutput, gamma) if the front
  // buffer is available; then it presents anyway: without a new refresh the
  // presenter re-presents the last output (black if there never was one),
  // so host_frames_presented = XE_SWAPs executed.
  //
  // Upscaler: the capture is valid only if the refresh happened in this IssueSwap (the base sent the
  // command list with the copies before the Present); after the Present the next frame's jitter.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    // Antialiasing changed by the overlay: on the command processor thread it already applies to this swap.
    if (const uint32_t aa = live_->antialiasing; aa != antialiasing_) {
      antialiasing_ = aa;
      SetDesiredSwapPostEffect(SwapPostEffect(aa));
    }
    const uint64_t refreshes = presenter_ ? presenter_->RefreshCount() : 0;
    D3D12CommandProcessor::IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
    const bool capture = presenter_ && presenter_->CaptureWanted();
    if (capture)
      // Several uber posts in the same frame (split screen): there is no capture per view, frame without upscaler.
      presenter_->SetUpscaleCapture(captures_ == 1 && presenter_->RefreshCount() != refreshes ? &capture_ : nullptr);
    // Once: a new use of the uber post (like the Tac-Com one) is recognized from this in the log.
    if (capture && captures_ > 1 && !multi_capture_logged_) {
      LogLine("[gpu] upscaler: " + std::to_string(captures_) +
              " uber posts in the same frame, frame without upscaler (split screen or unrecognized use)");
      multi_capture_logged_ = true;
    }
    captures_ = 0;
    ++swaps_;
    std::erase_if(retired_, [&](const auto& e) { return swaps_ - e.first > 3; });
    const bool presented = !presenter_ || presenter_->Present();
    // On every swap: without an upscaler (never active or off) NextJitter and MipBias give 0.
    if (presenter_) {
      const auto [x, y] = presenter_->NextJitter();
      capture_.jitter_x = x;
      capture_.jitter_y = y;
      const auto& rtc = Gow2RenderTargetCache();
      SetGow2Jitter(x / float(rtc.GetDrawScaleX()), y / float(rtc.GetDrawScaleY()), scene_pitch_);
      SetGow2MipBias(presenter_->MipBias());
    }
    if (presented) return;
    // The presenter has already written the reason (`[gpu] device removed ...` or the
    // failed call). Summary and exit 5 from the xe::FatalError hook.
    const bool removed = GetD3D12Provider().GetDevice()->GetDeviceRemovedReason() != S_OK;
    xe::FatalError(removed ? "D3D12 presentation failed: device removed; retry with --gpu fake"
                           : "D3D12 presentation failed; retry with --gpu fake");
  }

  // The last Present may still read the capture textures (released with the command processor): the base
  // waits only for Xenia's work.
  void ShutdownContext() override {
    if (presenter_) presenter_->WaitIdle();
    D3D12CommandProcessor::ShutdownContext();
  }

  // Resolve (copy from EDRAM to guest memory): counted in the summary.
  bool IssueCopy() override {
    kernel::stats::NoteGpuResolve();
    return D3D12CommandProcessor::IssueCopy();
  }

  // Upscaler: after UE3's uber post (motion blur, depth of field, tonemapping; then only the HUD) the
  // image without HUD, the depth (tf0) and the velocity buffer (tf3) are copied and the constants are read.
  bool IssueDraw(xe::gpu::xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    xe::gpu::Shader* ps = active_pixel_shader();  // already this draw's (set by the packets before)
    const bool uber_post = ps && ps->ucode_data_hash() == kUberPostHash;
    // [patches] disable_motion_blur: velocity scale to 0 only for this draw, c17.xy (camera, instruction
    // 26 of the ucode) and c18.xy (dynamic objects, instruction 14); restored immediately, the capture reads the real ones.
    uint32_t saved[4];
    const uint32_t blur_regs[4] = {kPsConst + 17 * 4, kPsConst + 17 * 4 + 1, kPsConst + 18 * 4, kPsConst + 18 * 4 + 1};
    const bool zero_blur = uber_post && live_->no_motion_blur;
    if (zero_blur)
      for (int i = 0; i < 4; ++i) {
        saved[i] = register_file_->values[blur_regs[i]];
        WriteRegister(blur_regs[i], 0);
      }
    const bool ok = D3D12CommandProcessor::IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit);
    if (zero_blur)
      for (int i = 0; i < 4; ++i) WriteRegister(blur_regs[i], saved[i]);
    // The Tac-Com adds a second uber post with 1x1 depth (no scene): it is not a view, it is not
    // captured (otherwise 2 captures = frame without upscaler, and without OptiScaler's FG).
    const auto t0 = register_file_->GetTextureFetch(0).size_2d;
    if (ok && presenter_ && presenter_->CaptureWanted() && uber_post && (t0.width || t0.height))
      CaptureUberPost();
    return ok;
  }

 private:
  static constexpr uint64_t kUberPostHash = 0xB6B6DD4E7D91F3D9ull;
  static constexpr uint32_t kPsConst = xe::gpu::XE_GPU_REG_SHADER_CONSTANT_256_X;  // pixel shader c0

  // w x h resource of format `format` in COPY_DEST (recreated if it changes); false if it cannot be created.
  bool EnsureCaptureTarget(Microsoft::WRL::ComPtr<ID3D12Resource>& r, uint32_t w, uint32_t h, DXGI_FORMAT format) {
    if (r) {
      const D3D12_RESOURCE_DESC d = r->GetDesc();
      if (d.Width == w && d.Height == h && d.Format == format) return true;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> fresh;
    if (FAILED(GetD3D12Provider().GetDevice()->CreateCommittedResource(
            &xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&fresh))))
      return false;
    // The old one may still be in use on the GPU (previous frame's Present): alive for 3 swaps.
    if (r) retired_.emplace_back(swaps_, r);
    r = fresh;
    return true;
  }

  void CaptureUberPost() {
    auto& rtc = Gow2RenderTargetCache();
    auto& tc = Gow2TextureCache();
    ID3D12Resource* color = rtc.Gow2ColorTarget0();
    ID3D12Resource* depth = tc.Gow2ActiveTexture(0);
    ID3D12Resource* velocity = tc.Gow2ActiveTexture(3);
    const auto& rf = *register_file_;
    const auto t0 = rf.GetTextureFetch(0), t3 = rf.GetTextureFetch(3);
    const uint32_t sx = rtc.GetDrawScaleX(), sy = rtc.GetDrawScaleY();
    const uint32_t w = (t0.size_2d.width + 1) * sx, h = (t0.size_2d.height + 1) * sy;
    // The velocity buffer (640 px) stays native with draw_resolution_scale_threshold: copied as is.
    const uint32_t vw = velocity ? CopySizeOfGuestTexture(t3.size_2d.width + 1, sx, velocity->GetDesc().Width) : 0;
    const uint32_t vh = velocity ? CopySizeOfGuestTexture(t3.size_2d.height + 1, sy, velocity->GetDesc().Height) : 0;
    const bool ok = color && depth && velocity &&
                    EnsureCaptureTarget(capture_.prehud, w, h, color->GetDesc().Format) &&
                    EnsureCaptureTarget(capture_.depth, w, h, depth->GetDesc().Format) &&
                    EnsureCaptureTarget(capture_.velocity, vw, vh, velocity->GetDesc().Format) &&
                    rtc.Gow2CopyColorTarget0(capture_.prehud.Get(), w, h) &&
                    tc.Gow2CopyActiveTexture(0, capture_.depth.Get(), w, h) &&
                    tc.Gow2CopyActiveTexture(3, capture_.velocity.Get(), vw, vh);
    if (!ok) {
      if (!capture_failed_logged_)
        LogLine("[gpu] upscaler: capture after the uber post failed (ROV path or textures missing), frame without upscaler");
      capture_failed_logged_ = true;
      return;
    }
    // Pixel shader constants: registers c256+ of the register file.
    auto read = [&](int c, float* out) {
      for (int k = 0; k < 4; ++k) out[k] = rf.Get<float>(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_256_X + c * 4 + k);
    };
    read(2, capture_.k.c2);
    read(13, capture_.k.c13);
    read(14, capture_.k.c14);
    read(15, capture_.k.c15);
    read(16, capture_.k.c16);
    read(17, capture_.k.c17);
    if (presenter_->HdrActive()) CaptureHdr(read);
    // Like Xenia's IssueSwap: PWL for a 2_10_10_10 front buffer, table otherwise (here from the format of the
    // uber post's render target, which is what ends up in the front buffer).
    const DXGI_FORMAT f = color->GetDesc().Format;
    capture_.ramp.pwl = f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R10G10B10A2_UNORM;
    static_assert(sizeof(xe::gpu::reg::DC_LUT_30_COLOR) == 4 && sizeof(xe::gpu::reg::DC_LUT_PWL_DATA) == 4);
    std::memcpy(capture_.ramp.table, gamma_ramp_256_entry_table(), sizeof(capture_.ramp.table));
    std::memcpy(capture_.ramp.pwl_rgb, gamma_ramp_pwl_rgb(), sizeof(capture_.ramp.pwl_rgb));
    scene_pitch_ = t0.size_2d.width + 1;  // guest width of the scene: pitch of its surfaces
    ++captures_;
  }

  // HDR10 output: tf1 (scene before tonemapping) and tf2 (bloom), uber post constants, filter and exp_adjust
  // of tf1..tf3. Failed copies: empty scene/bloom, frame without residual.
  template <typename Read>
  void CaptureHdr(Read& read) {
    auto& tc = Gow2TextureCache();
    const auto& rf = *register_file_;
    HdrUberConstants& k = capture_.hdr;
    for (int i = 0; i < 19; ++i) read(i, k.c[i]);
    read(253, k.c253);
    read(254, k.c254);
    read(255, k.c255);
    for (uint32_t t = 1; t <= 3; ++t) {
      const auto f = rf.GetTextureFetch(t);
      k.linear[t - 1] = f.mag_filter == xe::gpu::xenos::TextureFilter::kLinear;
      k.exp_scale[t - 1] = std::ldexp(1.0f, f.exp_adjust);
    }
    const uint32_t sx = Gow2RenderTargetCache().GetDrawScaleX(), sy = Gow2RenderTargetCache().GetDrawScaleY();
    ID3D12Resource* scene = tc.Gow2ActiveTexture(1);
    ID3D12Resource* bloom = tc.Gow2ActiveTexture(2);
    const auto t1 = rf.GetTextureFetch(1), t2 = rf.GetTextureFetch(2);
    const uint32_t w1 = (t1.size_2d.width + 1) * sx, h1 = (t1.size_2d.height + 1) * sy;
    const uint32_t w2 = (t2.size_2d.width + 1) * sx, h2 = (t2.size_2d.height + 1) * sy;
    const bool ok = scene && bloom && EnsureCaptureTarget(capture_.scene, w1, h1, scene->GetDesc().Format) &&
                    EnsureCaptureTarget(capture_.bloom, w2, h2, bloom->GetDesc().Format) &&
                    tc.Gow2CopyActiveTexture(1, capture_.scene.Get(), w1, h1) &&
                    tc.Gow2CopyActiveTexture(2, capture_.bloom.Get(), w2, h2);
    if (ok) return;
    // Old resources possibly still read by the previous frame's Present: alive for 3 swaps.
    if (capture_.scene) retired_.emplace_back(swaps_, capture_.scene);
    if (capture_.bloom) retired_.emplace_back(swaps_, capture_.bloom);
    capture_.scene.Reset();
    capture_.bloom.Reset();
    if (!hdr_capture_failed_logged_) LogLine("[hdr] capture of tf1/tf2 failed: frame without HDR lights");
    hdr_capture_failed_logged_ = true;
  }

  Presenter* presenter_;
  std::shared_ptr<LiveGpuSettings> live_;
  uint32_t antialiasing_;  // last one passed to SetDesiredSwapPostEffect
  UpscaleCapture capture_{};
  uint32_t captures_ = 0;  // uber posts captured in the current frame (capture_ is the last one)
  uint32_t scene_pitch_ = 0;  // scene surfaces for the jitter (0 = none until there is a capture)
  bool capture_failed_logged_ = false;
  bool multi_capture_logged_ = false;
  bool hdr_capture_failed_logged_ = false;
  uint64_t swaps_ = 0;
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;  // (swap, resource)
};

void ApplyBuiltinOverrides(xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov, bool bindless) {
  ApplyOverride(BuiltinShaderOverrides(), t, rov, bindless);
}

class D3D12Backend final : public Backend {
 public:
  D3D12Backend(std::unique_ptr<xe::Memory> memory, std::unique_ptr<xe::ui::d3d12::D3D12Provider> provider,
               std::unique_ptr<Presenter> presenter, std::shared_ptr<LiveGpuSettings> live)
      : memory_(std::move(memory)), provider_(std::move(provider)), presenter_(std::move(presenter)) {
    Presenter* p = presenter_.get();
    auto graphics_system =
        std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>(memory_.get(), provider_.get(), p);
    inner_ = CreateXeniaBackend(std::move(graphics_system),
                                [p, live](xe::gpu::GraphicsSystem* gs) -> std::unique_ptr<xe::gpu::CommandProcessor> {
                                  using Effect = xe::gpu::CommandProcessor::SwapPostEffect;
                                  static_assert(int(Effect::kNone) == 0 && int(Effect::kFxaa) == 1 &&
                                                int(Effect::kFxaaExtreme) == 2);
                                  return std::make_unique<Gow2D3D12CommandProcessor>(
                                      static_cast<xe::gpu::d3d12::D3D12GraphicsSystem*>(gs), p, live);
                                });
  }

  // First the command processor (thread stopped, then GraphicsSystem and CP
  // destroyed: it uses the presenter, device and xe::Memory callbacks), then
  // presenter, device and xe::Memory (removes the callbacks left over by physwatch).
  ~D3D12Backend() override {
    inner_.reset();
    xe::gpu::d3d12::SetTranslatedPixelShaderHook(nullptr);  // global: does not stay for the following backends
    presenter_.reset();
    provider_.reset();
    memory_.reset();
  }

  bool valid() const { return inner_ != nullptr; }

  void ArmScreenshot(std::chrono::steady_clock::time_point at, std::filesystem::path file) {
    if (presenter_) presenter_->ArmScreenshot(at, std::move(file));
  }

  bool Start() override { return inner_->Start(); }
  void Stop() override { inner_->Stop(); }
  void InitializeRingBuffer(uint32_t phys, uint32_t size_log2) override {
    inner_->InitializeRingBuffer(phys, size_log2);
  }
  void EnableReadPointerWriteBack(uint32_t phys, uint32_t block_size_log2) override {
    inner_->EnableReadPointerWriteBack(phys, block_size_log2);
  }
  // Xenia MarkVblank: increment_counter of the CP.
  void OnVblank() override { inner_->OnVblank(); }
  Counters GetCounters() const override { return inner_->GetCounters(); }

 private:
  std::unique_ptr<xe::Memory> memory_;
  std::unique_ptr<xe::ui::d3d12::D3D12Provider> provider_;
  std::unique_ptr<Presenter> presenter_;
  std::unique_ptr<Backend> inner_;
};

}  // namespace

std::unique_ptr<Backend> CreateD3D12Backend(GuestMemory& mem, const D3D12Options& options) {
  const int32_t selector = cvars::d3d12_adapter;
  const auto adapter = FindAdapter(selector);
  if (!adapter) {
    LogLine("[gpu] no D3D12 adapter (feature level 11_0) for d3d12_adapter=" + std::to_string(selector));
    return nullptr;
  }
  auto provider = xe::ui::d3d12::D3D12Provider::Create();  // xe::FatalError on failure
  if (!provider) return nullptr;
  // Upscaler (gpu/upscaler.h): replaces FXAA and CAS; the sharpness goes to RCAS. If it is not available (DLL,
  // driver, DLSS in this build, ROV path) the game is played as with off: it is tried right away.
  // The DLLs (upscaler, frame generation) are searched only next to gow2.exe.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
  UpscalerFactory factory;
  if (options.upscaler != 0 && options.window) {
    ID3D12Device* device = provider->GetDevice();
    const bool fsr = options.upscaler == 1;
    const settings::DlssPreset preset = options.dlss_preset;
    factory = [device, dir, fsr, preset](uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
      return fsr ? CreateFsrUpscaler(device, rw, rh, ow, oh, dir)
                 : CreateDlssUpscaler(device, rw, rh, ow, oh, dir, preset);
    };
    if (cvars::render_target_path_d3d12 == "rov") {
      LogLine("[gpu] upscaler not available with the ROV path: playing as with off");
      factory = nullptr;
    } else if (std::shared_ptr<Upscaler> probe = factory(1280, 720, 1280, 720); !probe) {
      LogLine("[gpu] upscaler not available: playing as with off");
      factory = nullptr;
    } else {
      // With DLSS the probe stays alive as long as the factory: the bridge keeps NGX open while there is a context, and the
      // presenter that destroys and recreates its own does not close and reopen it (NGX proxies like OptiScaler crash).
      // With FSR it is released right away (a useless FFX context).
      factory = [inner = std::move(factory), keep = fsr ? nullptr : probe](uint32_t rw, uint32_t rh, uint32_t ow,
                                                                          uint32_t oh) {
        return inner(rw, rh, ow, oh);
      };
    }
  }
  const bool upscaler = bool(factory);
  if (upscaler && (options.antialiasing || options.sharpening))
    LogLine("[gpu] upscaler active: FXAA and CAS ignored, sharpness with the upscaler's RCAS");
  const uint32_t antialiasing = upscaler ? 0 : options.antialiasing;
  auto live = std::make_shared<LiveGpuSettings>();
  live->antialiasing = antialiasing;
  live->no_motion_blur = options.settings && options.settings->patches.contains("disable_motion_blur");
  std::unique_ptr<Presenter> presenter;
  if (options.window) {
    PresentOptions present{0, 0, options.stretch, options.vsync};
    present.exclusive_fullscreen = options.exclusive_fullscreen;
    present.mode_width = options.mode_width;
    present.mode_height = options.mode_height;
    present.sharpening = options.sharpening && !upscaler;
    present.sharpness = options.sharpness;
    present.exclusive_wanted = [w = options.window](bool active) { return plat::ExclusiveFullscreenWanted(w, active); };
    present.frame_rate = options.frame_rate;
    present.vrr = options.vrr;
    if (options.frame_generation != settings::FrameGeneration::kOff) {
      if (cvars::render_target_path_d3d12 == "rov")
        LogLine("[fg] frame generation not available (ROV path): normal swap chain");
      else
        present.frame_generator = [dir, dlss = options.frame_generation == settings::FrameGeneration::kDlss](
                                      const SwapChainRequest& r, Microsoft::WRL::ComPtr<IDXGISwapChain4>& out) {
          return dlss ? CreateDlssFrameGenerator(r, out, dir) : CreateFsrFrameGenerator(r, out, dir);
        };
    }
    // Without NVAPI the query costs microseconds and stays on the Present thread; with NVAPI (4-5 ms) it goes to
    // SyncPoller, created here (first query at startup, before the first Present) if it is already needed now,
    // otherwise at the first switch to "auto" from the overlay (that time synchronous on the Present thread).
    std::shared_ptr<frame_pacing::SyncPoller> poller;
    const auto query_nvapi = [w = options.window] { return plat::QueryDisplaySync(w, true); };
    if (options.frame_rate != settings::FrameRate::k30 && options.vrr == settings::Vrr::kAuto)
      poller = std::make_shared<frame_pacing::SyncPoller>(query_nvapi);
    present.display_sync = [w = options.window, query_nvapi, poller](bool nvapi) mutable {
      if (!nvapi) return plat::QueryDisplaySync(w, false);
      if (!poller) poller = std::make_shared<frame_pacing::SyncPoller>(query_nvapi);
      return poller->Get();
    };
    std::string hdr_line;  // chosen levels: written only if the presenter really turns HDR on
    if (options.hdr) {
      if (cvars::render_target_path_d3d12 == "rov") {
        LogLine("[hdr] HDR not available (ROV path): SDR");
      } else {
        const hdr::Display display = launcher::QueryHdrDisplay(plat::NativeHandle(options.window));
        char line[320];
        if (display.hdr || options.hdr_force) {
          present.hdr = hdr::ChooseLevels(options.hdr_peak_nits, options.hdr_paper_white_nits, display);
          std::snprintf(line, sizeof line,
                        "[hdr] HDR10: peak %.0f nit, paper white %.0f nit; output %s %s, MaxLuminance %.0f, "
                        "SDR white %.0f nit%s",
                        present.hdr->peak_nits, present.hdr->paper_white_nits, display.name.c_str(),
                        display.hdr ? "HDR" : "SDR", display.max_nits, display.sdr_white_nits,
                        display.hdr ? "" : " (--hdr force)");
          hdr_line = line;
        } else {
          std::snprintf(line, sizeof line, "[hdr] output not HDR (%s; Windows HDR off?): SDR",
                        display.name.c_str());
          LogLine(line);
        }
      }
    }
    presenter = Presenter::Create(*provider, plat::NativeHandle(options.window), present);
    if (!presenter) {
      LogLine("[gpu] cannot create the swap chain on the game window");
      return nullptr;
    }
    if (!hdr_line.empty() && presenter->HdrActive()) LogLine(hdr_line);
  }
  if (presenter && options.settings) {
    Presenter* p = presenter.get();
    presenter->SetOverlay(Overlay::Create(
        options.window, options.config, *options.settings,
        [p, w = options.window, aniso = options.settings->video.anisotropic, &mem, live, upscaler,
         run = *options.settings](const settings::Settings& s) mutable {
          const launcher::WindowSetup ws = launcher::WindowSetupFor(s.video);
          p->SetVideo({s.video.vsync, s.video.scaling == settings::Scaling::kStretch, s.video.sharpening,
                       s.video.sharpness, ws.exclusive, ws.mode_width, ws.mode_height, s.video.vrr,
                       s.video.frame_generation != settings::FrameGeneration::kOff});
          plat::ApplyWindowOptions(w, ws.window);
          // Hot: the cvar is read on every draw. It is written only if the menu choice changed: then it is
          // the user's last word and wins over [gpu].cvars / --gpu-cvar anisotropic_override ("Game default"
          // goes back to -1); if it did not change (e.g. only V-Sync) a value set by those stays.
          if (s.video.anisotropic != aniso) {
            aniso = s.video.anisotropic;
            SetXeniaCvar("anisotropic_override", std::to_string(settings::AnisotropicOverride(aniso)));
          }
          // Hot like the anisotropic: only what changed compared with the running state is touched.
          using game_patches::PatchId;
          for (const auto& [key, id] : {std::pair{"show_fps", PatchId::kShowFps},
                                        std::pair{"disable_lens_flares", PatchId::kDisableLensFlares},
                                        std::pair{"disable_bloom", PatchId::kDisableBloom}})
            if (s.patches.contains(key) != run.patches.contains(key))
              game_patches::SetEnabled(mem, id, s.patches.contains(key));
          if (const bool on = s.patches.contains("disable_motion_blur"); on != run.patches.contains("disable_motion_blur"))
          {
            live->no_motion_blur = on;
            LogLine(std::string("[patch] disable_motion_blur: ") + (on ? "turned on hot" : "turned off hot"));
          }
          const settings::Video &a = run.video, &b = s.video;
          if (!upscaler && a.antialiasing != b.antialiasing) live->antialiasing = uint32_t(b.antialiasing);
          run.patches = s.patches;
          run.video = s.video;
        }));
  }
  if (upscaler) {
    const std::string& d = cvars::gow2_upscaler_debug;
    const UpscaleDebug debug = d == "ffx_debug_view" ? UpscaleDebug::kFfxDebugView : UpscaleDebug::kNone;
    presenter->SetUpscaler(std::move(factory), options.sharpening ? options.sharpness : -1.0f, debug);
  }
  // Before the command processor, which creates the pipeline cache. With false the hook
  // is not touched (in the game it stays nullptr; tests may have set their own).
  if (options.enhanced_shaders) xe::gpu::d3d12::SetTranslatedPixelShaderHook(&ApplyBuiltinOverrides);
  const bool frame_generation = presenter && presenter->FrameGenerationActive();
  const bool hdr10 = presenter && presenter->HdrActive();
  auto backend = std::make_unique<D3D12Backend>(std::make_unique<xe::Memory>(mem.base), std::move(provider),
                                                std::move(presenter), live);
  if (!backend->valid()) {
    LogLine("[gpu] cannot create Xenia's D3D12 command processor");
    return nullptr;
  }
  for (const ScreenshotRequest& shot : options.screenshots)
    backend->ArmScreenshot(options.start + std::chrono::seconds(shot.seconds), shot.file);
  std::string window = "no window";
  if (options.window) {
    const auto [w, h] = plat::ClientSize(options.window);
    window = "window " + std::to_string(w) + "x" + std::to_string(h) + ", back buffer like the window" +
             (options.stretch ? ", stretch" : ", fit") + (options.vsync ? ", vsync" : ", no vsync") +
             (options.exclusive_fullscreen ? ", exclusive fullscreen" : "");
  }
  window += ", frame rate " + std::string(settings::FrameRateName(options.frame_rate)) + ", vrr " +
            (options.vrr == settings::Vrr::kAuto ? "auto" : options.vrr == settings::Vrr::kOn ? "on" : "off");
  static const char* const kAaNames[] = {"off", "fxaa", "fxaa_extreme"};
  window += std::string(", antialiasing ") + kAaNames[antialiasing <= 2 ? antialiasing : 0];
  if (upscaler) window += options.upscaler == 1 ? ", upscaler fsr" : ", upscaler dlss";
  if (frame_generation) window += ", frame generation";
  if (hdr10) window += ", HDR10";
  if (cvars::draw_resolution_scale_x > 1)
    window += ", internal resolution " + std::to_string(cvars::draw_resolution_scale_x) + "x";
  // Effective value of the cvar (--gpu-cvar and [gpu].cvars included): 2..5 = 2x..16x, other = the game's.
  window += ", anisotropic " + std::string(settings::AnisotropicOverrideName(cvars::anisotropic_override));
  window += options.enhanced_shaders ? ", enhanced shaders on" : ", enhanced shaders off";
  LogLine("[gpu] D3D12: adapter " + *adapter + ", " + window +
          ", render_target_path_d3d12=" + cvars::render_target_path_d3d12 +
          ", Xenia command processor");
  return backend;
}

void SetFatalErrorHook(void (*hook)()) { gow2::xenia::SetFatalErrorHook(hook); }

bool SetXeniaCvar(std::string_view name, std::string_view value) { return gow2::xenia::SetCvar(name, value); }

}  // namespace gpu
