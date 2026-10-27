// gow2 DXGI presenter: swap chain on the game
// window, guest output written by the command processor, BMP screenshots.
//
// Takes the place of Xenia's D3D12Presenter (not vendored): derives
// from the shim xenia/ui/d3d12/d3d12_presenter.h, so that
// D3D12CommandProcessor::IssueSwap uses it unmodified.
//
//  - Swap chain (Global Constraints): flip-discard, 2 buffers,
//    kGuestOutputFormat (R10G10B10A2_UNORM), DXGI_SCALING_STRETCH (fits a
//    fixed-size back buffer to the window), DXGI's Alt-Enter
//    disabled (the window handles it).
//  - Back buffer (PresentOptions): width x height if non-zero, otherwise
//    the window's client area, reread (GetClientRect) on every Present: if
//    it changes, GPU wait, ResizeBuffers, RTVs and readback recreated; if it fails
//    a [gpu] line and the previous size is kept. Client area 0x0
//    (minimized): Present skipped, frame not counted.
//  - Vsync: Present(1, 0); without vsync Present(0, DXGI_PRESENT_ALLOW_TEARING)
//    if DXGI supports tearing (swap chain created with the flag), otherwise
//    Present(0, 0). In exclusive fullscreen the tearing flag is not used
//    (DXGI_ERROR_INVALID_CALL).
//  - Exclusive fullscreen (PresentOptions::exclusive_fullscreen): on every
//    Present, before resizing, the wanted state (exclusive_wanted)
//    is compared with GetFullscreenState. Entry: FindClosestMatchingMode
//    on the window's output (mode_width x mode_height and current frequency;
//    0 = current mode), SetFullscreenState(TRUE), ResizeTarget (changes the mode:
//    swap chain created with DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH); exit
//    SetFullscreenState(FALSE), DXGI restores the desktop mode. This way it re-enters after DXGI left on its
//    own (Alt-Tab). Every state change forces ResizeBuffers (on the client area,
//    which DXGI brought to the mode). Error: [gpu] line, it stays borderless and
//    does not retry (except DXGI_ERROR_NOT_CURRENTLY_AVAILABLE and
//    DXGI_STATUS_MODE_CHANGE_IN_PROGRESS, transient). Destruction:
//    SetFullscreenState(FALSE) before releasing the swap chain.
//  - Guest output: kGuestOutputFormat texture usable as a UAV,
//    recreated when the size changes. The refresher receives it in
//    kGuestOutputInternalState (PIXEL_SHADER_RESOURCE) and gives it back to us
//    in the same state (Xenia's contract), using the provider's direct
//    queue.
//  - Present: black back buffer, then the last guest output: if it has the
//    back buffer's dimensions and sharpening is off CopyTextureRegion
//    (same format, no shader, identical bytes); otherwise a
//    rectangle drawn with Xenia's shaders (guest_output_triangle_strip_rect_vs
//    and the effect's pixel shader, root signature and constants of Xenia's
//    D3D12Presenter) in the FitRect rectangle (fit:
//    guest output aspect with black bands; stretch: everything). A Present without
//    a new RefreshGuestOutput re-presents the last output (IssueSwap
//    presents also when the front buffer is not available); the frame is
//    black only if no refresh ever succeeded or the last one failed.
//  - Effect (ChoosePaintEffect): bilinear without sharpening; with
//    PresentOptions::sharpening AMD FidelityFX CAS (sharpen at 1:1,
//    resample between 1/2 and 2x per axis), outside that ratio bilinear with
//    a [gpu] line the first time.
//    shortcut: beyond 2x no CAS, upgrade: intermediate pass like Xenia's D3D12Presenter.
//    shortcut: with CAS upscaling the 1 px border comes out darker (Xenia's CasLoad without clamp, reads outside the texture = 0), upgrade: shader recompiled with a clamped CasLoad.
//  - Screenshot: requests queued, ordered by instant; every Present serves
//    at most the first due request (now >= at), so each one comes out of the
//    first Present from `at` onwards that is left free, in time order. Copy
//    of the back buffer into a readback buffer, fence wait, 10 -> 8 bit
//    (v >> 2), 24-bit BMP; line `[gpu] screenshot <file> <w>x<h> frame host <n>`.
//
//  - F1 overlay (gpu/overlay.h, SetOverlay): drawn over the guest output, before the screenshot.
//  - SetVideo (the overlay's Apply): vsync, stretch, sharpening, exclusive fullscreen and its video mode
//    from the next Present. The swap chain has DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING whenever DXGI
//    supports it, so vsync can be turned off hot.
//  - Frame generation (PresentOptions::frame_generator): it draws into the generator's buffers (GetBuffer,
//    CurrentBufferIndex) and presents with its Present; per-frame inputs from the capture, WaitForPresents
//    before ResizeBuffers and buffers requested again after, Shutdown on destruction; no exclusive.
//  - Frame rate (PresentOptions::frame_rate, display_sync): limiter right before Present, lines
//    `[frame] monitor ...` when frequency, VRR state or limit change.
//
// Threads: RefreshGuestOutput and Present from the same thread (the command
// processor's); ArmScreenshot and FramesPresented from any thread.
// Fence waits use SetEventOnCompletion(value, nullptr): no OS
// event.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "gpu/frame_generation.h"
#include "gpu/frame_generation_state.h"
#include "gpu/hdr_passes.h"
#include "gpu/overlay.h"
#include "gpu/upscale_frame_state.h"
#include "gpu/upscale_passes.h"
#include "gpu/upscaler.h"
#include "frame_pacing.h"
#include "hdr_math.h"
#include "platform/platform.h"
#include "settings.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace gpu {

// Capture of a frame for the upscaler, made by the command processor after
// UE3's uber post: image without HUD, depth and velocity buffer at internal resolution (resources in
// COPY_DEST state outside the Present), reprojection constants, gamma ramp, jitter of the frame's draws.
struct UpscaleCapture {
  Microsoft::WRL::ComPtr<ID3D12Resource> prehud, depth, velocity;
  UberPostConstants k;
  GammaRamp ramp;
  float jitter_x, jitter_y;  // render pixels
  // tf1, tf2: only with HDR (COPY_DEST outside the Present), empty = no residual
  Microsoft::WRL::ComPtr<ID3D12Resource> scene, bloom;
  HdrUberConstants hdr;  // gpu/hdr_passes.h
};
// Creates the upscaler for render rw x rh and maximum output ow x oh; nullptr = not available (already in the log).
using UpscalerFactory =
    std::function<std::unique_ptr<Upscaler>(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)>;
// --gpu-cvar gow2_upscaler_debug: ffx_debug_view asks the upscaler for its debug view.
enum class UpscaleDebug { kNone, kFfxDebugView };

struct PresentOptions {
  uint32_t width = 0, height = 0;  // 0 = window client area
  bool stretch = false;            // false = fit (guest output aspect, black bands)
  bool vsync = true;
  bool exclusive_fullscreen = false;
  uint32_t mode_width = 0, mode_height = 0;  // exclusive's video mode; 0 = the monitor's current mode
  // Exclusive wanted now, given the current state (called on every Present);
  // empty = always. gow2: plat::ExclusiveFullscreenWanted.
  std::function<bool(bool active)> exclusive_wanted;
  bool sharpening = false;
  float sharpness = 0.0f;  // 0..1, CAS "additional sharpness"
  // Frame rate: with k30 or empty display_sync no monitor query
  // and no limiter. Otherwise display_sync(vrr == kAuto) at the first Present, every 2 s (500 ms with
  // kAuto) and after SetVideo; the limit (frame_pacing::LimiterHz) is waited for right before swap_chain_->Present.
  settings::FrameRate frame_rate = settings::FrameRate::k30;
  settings::Vrr vrr = settings::Vrr::kAuto;
  std::function<plat::DisplaySync(bool nvapi)> display_sync;  // gow2: plat::QueryDisplaySync
  // Frame generation: proxy swap chain only from this factory; empty =
  // normal swap chain. If it returns nullptr: normal swap chain (fallback, [fg] line). With the proxy no exclusive
  // and limit FrameGenLimiterHz while generation is on.
  FrameGeneratorFactory frame_generator;
  // HDR10 output: swap chain in G2084 BT.2020, SDR image
  // in an intermediate composed in PQ with the capture's residual. Empty = SDR as before; color space rejected
  // or passes not created: [hdr] line and SDR.
  std::optional<hdr::Levels> hdr;
};

// Presenter settings applicable hot (Presenter::SetVideo).
struct VideoChange {
  bool vsync = true, stretch = false, sharpening = false;
  float sharpness = 0.0f;
  bool exclusive_fullscreen = false;
  uint32_t mode_width = 0, mode_height = 0;
  settings::Vrr vrr = settings::Vrr::kAuto;
  bool frame_generation = false;  // generation on (with the proxy created at startup)
};


struct Rect {
  uint32_t x, y, w, h;
  bool operator==(const Rect&) const = default;
};

// Destination rectangle of a src_w x src_h image in dst_w x dst_h.
// stretch: all of dst. fit: the largest with src's aspect (computed side
// truncated), centered (offset truncated). Zero dimensions -> {0, 0, 0, 0}.
Rect FitRect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, bool stretch);

// Shader for drawing the guest output (index of the presenter's pipelines).
enum class PaintEffect { kBilinear, kCasSharpen, kCasResample };
// Without sharpening kBilinear; with sharpening kCasSharpen if dst has
// src's dimensions, kCasResample if on both axes src/2 <= dst <= 2*src
// (limit of Xenia's CAS in one pass), otherwise kBilinear.
PaintEffect ChoosePaintEffect(uint32_t src_w, uint32_t src_h, Rect dst, bool sharpening);

class Presenter final : public xe::ui::d3d12::D3D12Presenter {
 public:
  // Swap chain on the window `hwnd` (see PresentOptions); nullptr (with a log
  // line) if a D3D12/DXGI call fails. The provider must outlive the
  // presenter.
  static std::unique_ptr<Presenter> Create(const xe::ui::d3d12::D3D12Provider&, void* hwnd,
                                           const PresentOptions& options = {});
  // Waits for the GPU to finish the presenter's work.
  ~Presenter() override;

  // Xenia Presenter::RefreshGuestOutput: false without calling the refresher if
  // the dimensions are zero or the texture cannot be created; otherwise the value
  // of the refresher (true = guest output presented by the following Presents).
  bool RefreshGuestOutput(uint32_t fb_w, uint32_t fb_h, uint32_t ax, uint32_t ay,
                          std::function<bool(GuestOutputRefreshContext&)> refresher) override;
  // Last guest output (black if absent) -> back buffer, Present,
  // kernel::stats::NoteHostFramePresented. true without presenting if the client
  // area is 0x0. false if the device is lost (with the line
  // `[gpu] device removed reason=0x%08X`) or a call fails.
  bool Present();
  // Queues a request: the first Present from `at` onwards (after those with
  // earlier or equal instant already queued) saves the screenshot to `file`.
  void ArmScreenshot(std::chrono::steady_clock::time_point at, std::filesystem::path file);
  uint64_t FramesPresented() const { return frames_presented_.load(); }

  // Upscaler. factory is called at the first capture and when the output
  // exceeds the context's maximum (after WaitIdle); if it returns nullptr the upscaler is turned off forever.
  // sharpness: RCAS 0..1, < 0 off.
  void SetUpscaler(UpscalerFactory factory, float sharpness, UpscaleDebug debug);
  bool UpscalerActive() const { return bool(upscaler_factory_); }
  // Proxy swap chain of frame generation created.
  bool FrameGenerationActive() const { return bool(frame_gen_); }
  // The command processor captures the frame (UpscaleCapture) for the upscaler or for frame generation.
  // Read by the command processor on the same thread as ApplyVideo (the Present runs inside IssueSwap).
  bool CaptureWanted() const { return UpscalerActive() || (frame_gen_ && frame_gen_enabled_) || HdrActive(); }
  // PresentOptions::hdr present, HDR10 color space accepted and passes created.
  bool HdrActive() const { return bool(hdr_levels_); }
  // Capture of the frame for the next Present (nullptr: frame without uber post, guest output as without
  // upscaler and history reset at the next capture). A Present consumes the capture: without a new one
  // the guest output is drawn.
  void SetUpscaleCapture(const UpscaleCapture* capture);
  // Jitter (render pixels) for the next frame's draws: (0, 0) until the upscaler is created, if it is off
  // or if the last Present did not reconstruct (frame without capture: the guest output must not shake).
  std::pair<float, float> NextJitter();
  // LOD bias of the scene textures for the same draws as the jitter: UpscalerMipBias(render, output) under the
  // conditions in which NextJitter gives a jitter, otherwise 0.
  float MipBias() const;
  // RefreshGuestOutput successes (the command processor works out whether its capture was sent to the GPU).
  uint64_t RefreshCount() const { return refresh_count_; }
  // Waits for the GPU to finish the presenter's work (the command processor, before releasing the
  // capture textures that the last Present may still read).
  void WaitIdle() { WaitFor(Signal()); }

  // F1 overlay, drawn in every Present (does nothing with the panel closed). Present thread.
  void SetOverlay(std::unique_ptr<Overlay> overlay);
  // Records new settings, applied at the start of the next Present. Thread of the Present.
  // Exclusive active and video mode (or exclusive) changed: it exits and UpdateFullscreen re-enters if wanted.
  // exclusive_fullscreen is no longer turned off once on (exclusive_wanted decides).
  void SetVideo(const VideoChange& change) { pending_video_ = change; }
  const PresentOptions& FrameOptions() const { return options_; }

 private:
  static constexpr uint32_t kBufferCount = 2;

  explicit Presenter(const xe::ui::d3d12::D3D12Provider& provider) : provider_(provider) {}
  bool Initialize(void* hwnd, const PresentOptions& options);
  bool CreatePaintPipeline();
  // Swap chain back buffers, dimensions and RTVs.
  bool AcquireBuffers();
  // Window client area (0x0 if minimized).
  std::pair<uint32_t, uint32_t> ClientArea() const;
  // Brings the back buffers to client_w x client_h (client area or fixed size).
  // false if the back buffers cannot be obtained again.
  bool ResizeIfNeeded(uint32_t client_w, uint32_t client_h);
  // Draws the texture of SRV srv (kSrvGuestOutput or kSrvUpscaled), src_w x src_h, in the fit/stretch rectangle.
  void DrawGuestOutput(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t srv, uint32_t src_w, uint32_t src_h);
  // Guest output (with the HUD) rebuilt by the upscaler, with the HUD reactive mask, and drawn; false
  // (nothing drawn) if the upscaler is not available or fails (then it is turned off forever, jitter
  // included): the caller draws the guest output.
  bool DrawUpscaled(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t slot);
  bool EnsureUpscaleTargets(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh);
  // Frame generation inputs from capture c (motion vectors, HUD mask, back buffer HUD-less
  // slot) and frame_gen_->Frame with them; HUD-less not created: Frame without generation.
  // hdr_residual: the frame's residual (PIXEL_SHADER_RESOURCE) or nullptr; with HDR the HUD-less goes to PQ.
  void FrameGenWithInputs(const UpscaleCapture& c, uint32_t slot, bool upscaled, const FrameGenFrame& f,
                          ID3D12Resource* hdr_residual);
  // With the proxy exclusive is not activated: exclusive to false, [fg] line the first time it was requested.
  void RefuseExclusiveWithFrameGen(bool& exclusive);
  // Signals the fence on the direct queue and returns the value.
  uint64_t Signal();
  void WaitFor(uint64_t value);
  bool EnsureReadback();
  // With pq also <file>.pq.ppm from readback_pq_.
  void SaveScreenshot(const std::filesystem::path& file, uint64_t frame, bool pq);
  // hdr_sdr_ as large as the back buffer (with its RTV); false after DisableHdr.
  bool EnsureHdrSdr();
  // Single SDR fallback: line "[hdr] <why>: SDR", HdrActive() false, G22 color space. Present thread.
  void DisableHdr(const char* why);
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const;
  void LogDeviceRemoved();
  // Exclusive fullscreen: enters or exits according to exclusive_wanted.
  void UpdateFullscreen();
  bool EnterFullscreen();

  const xe::ui::d3d12::D3D12Provider& provider_;
  void* hwnd_ = nullptr;
  PresentOptions options_;
  UINT swap_chain_flags_ = 0;  // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING if used
  uint32_t width_ = 0, height_ = 0;
  // Last client area for which ResizeBuffers failed: it is not retried on every frame.
  std::pair<uint32_t, uint32_t> resize_failed_{};
  bool exclusive_ = false;            // last GetFullscreenState
  bool exclusive_failed_ = false;     // entry failed: not retried
  Microsoft::WRL::ComPtr<IDXGISwapChain4> swap_chain_;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> back_buffers_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  uint32_t rtv_size_ = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kBufferCount> allocators_;
  std::array<uint64_t, kBufferCount> allocator_fence_{};
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;

  // Guest output drawing, for PaintEffect: SRVs in a shader-visible heap,
  // written when the texture is created (the previous one is no longer in use).
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 3> paint_root_signatures_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3> paint_pipelines_;
  static constexpr uint32_t kSrvGuestOutput = 0, kSrvUpscaled = 1;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
  uint32_t srv_size_ = 0;
  bool cas_fallback_logged_ = false;  // line of the fallback to bilinear already written

  // Guest output (command processor thread).
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_;
  uint32_t guest_width_ = 0, guest_height_ = 0;
  bool guest_output_valid_ = false;  // the last refresh succeeded

  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint_{};

  std::mutex screenshot_mutex_;
  // Ordered by instant (on ties, in arrival order).
  std::vector<std::pair<std::chrono::steady_clock::time_point, std::filesystem::path>> screenshots_;

  std::atomic<uint64_t> frames_presented_{0};
  uint64_t refresh_count_ = 0;

  // Upscaler (command processor thread).
  UpscalerFactory upscaler_factory_;
  float upscale_sharpness_ = -1.0f;
  UpscaleDebug upscale_debug_ = UpscaleDebug::kNone;
  std::unique_ptr<Upscaler> upscaler_;
  std::unique_ptr<UpscalePasses> upscale_passes_;
  uint32_t upscaler_max_[4] = {};  // render w h, output w h of the context
  std::optional<UpscaleCapture> capture_;
  UpscaleFrameState upscale_state_;
  // R16G16_FLOAT, R8_UNORM, kGuestOutputFormat; in UAV outside DrawUpscaled.
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_, reactive_, upscaled_;
  // kGuestOutputFormat in UAV: input without HUD (render) and upscaler output before the HUD (output).
  Microsoft::WRL::ComPtr<ID3D12Resource> scene_in_, upscaled_scene_;
  uint32_t jitter_index_ = 0, jitter_render_w_ = 0, jitter_output_w_ = 0;
  std::chrono::steady_clock::time_point last_upscale_;
  bool upscaled_last_ = false;  // the last Present drew the upscaler output

  std::optional<VideoChange> pending_video_;
  void ApplyVideo(const VideoChange& change);

  // Frame rate (Present thread): queries the monitor if due and waits for the limit.
  void PaceFrame();
  frame_pacing::FrameLimiter limiter_;
  std::chrono::steady_clock::time_point last_sync_{};  // {} = query at the next Present
  double limiter_hz_ = 0;
  plat::DisplaySync display_{};  // last monitor query
  // frequency, VRR active, limit, frame generation on
  std::optional<std::tuple<uint32_t, bool, double, bool>> last_logged_;

  // Frame generation (Present thread).
  std::unique_ptr<FrameGenerator> frame_gen_;
  bool frame_gen_enabled_ = false;
  FrameGenState frame_gen_state_;
  bool fg_exclusive_logged_ = false;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> hudless_;  // one per back buffer, R10G10B10A2

  // HDR10 output (Present thread). RTV: back buffer 0..1, hdr_sdr_ 2, hdr_hudless_ 3..4.
  std::optional<hdr::Levels> hdr_levels_;  // only if color space and passes succeeded
  std::unique_ptr<HdrPasses> hdr_passes_;
  // R10G10B10A2 as large as the back buffer: SDR image (RTV, at rest COMMON) and its copy before the overlay
  // (at rest COPY_DEST, created at the first opening).
  Microsoft::WRL::ComPtr<ID3D12Resource> hdr_sdr_, hdr_pre_overlay_;
  Microsoft::WRL::ComPtr<ID3D12Resource> hdr_residual_;  // RGBA16F at render resolution, at rest UAV
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> hdr_hudless_;  // HUD-less in PQ, RTV
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_pq_;  // PQ back buffer of the screenshot (footprint of readback_)
  // Last member: destroyed first, with the GPU idle (~Presenter waits).
  std::unique_ptr<Overlay> overlay_;
};

}  // namespace gpu
