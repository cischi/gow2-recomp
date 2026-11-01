// See d3d12_backend.h.
#include "gpu/d3d12_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "game_patches/game_patches.h"
#include "game_profile.h"
#include "gpu/frame_generation.h"
#include "gpu/presenter.h"
#include "launcher/launcher.h"
#include "launcher/launcher_model.h"
#include "launcher/optiscaler.h"
#include "gpu/png_writer.h"
#include "gpu/shader_capture.h"
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

// --shader-capture (gpu/shader_capture.h): F11 on the window thread asks for a start/stop, applied by the command
// processor at the next swap (the recording never starts or stops mid-frame). Draws come from Xenia's draw observer
// (patch 0021) on the command processor thread; shots go through the presenter's readback ring to the write queue.
struct ShaderCapture {
  ShaderCaptureSession session;
  std::unique_ptr<ImageWriteQueue> queue;  // created at the first start: no threads without a recording
  std::atomic<bool> toggle{false};
  std::filesystem::path root;  // folder of gow2.exe (like F12's screenshots)
  Overlay* overlay = nullptr;
  const xe::ui::d3d12::D3D12Provider* provider = nullptr;
  uint64_t written_base = 0, dropped_base = 0;  // counters at the start of the recording
  uint32_t depth_control = 0, mode_control = 0, color_mask = 0, depth_info = 0;  // of the draw being issued
  std::array<uint32_t, 32> regs{};
  std::ofstream all_frames;  // <dir>/frames_all.txt, command processor thread
  std::filesystem::path all_frames_dir;
  // Debug frames (1 in kDebugFrameEvery, numbered as in frames_all.txt from 0): the query boxes (NoteQueryBox) go to
  // dbg/<frame>.boxes.txt, the depth texture behind them (CaptureQueryDepth) to dbg/<frame>.depth<k>.bin.
  static constexpr uint32_t kDebugFrameEvery = 15;
  uint32_t frame_index = 0;
  bool DebugFrame() const { return frame_index % kDebugFrameEvery == 0; }
  std::string boxes, box_stream_text;
  std::set<uint32_t> box_streams;

  // First use of (hash, modification) in this recording: ucode, DXBC and its disassembly into shaders/.
  void Dump(xe::gpu::d3d12::D3D12Shader::D3D12Translation& t) {
    // Invalid translation: nothing to dump, and not marked as seen.
    if (!t.is_valid() || !session.FirstSeen(t.shader().ucode_data_hash(), t.modification())) return;
    const std::filesystem::path dir = session.Dir() / "shaders";
    t.shader().DumpUcode(dir);
    const auto [binary, disasm] = t.Dump(dir, "d3d12");
    if (!disasm.empty() || binary.empty() || !provider) return;
    // Xenia disassembles only with d3d12_dxbc_disasm: done here, without touching the translation.
    ID3DBlob* blob = nullptr;
    const auto& dxbc = t.translated_binary();
    if (FAILED(provider->Disassemble(dxbc.data(), dxbc.size(),
                                     D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING | D3D_DISASM_ENABLE_INSTRUCTION_OFFSET,
                                     nullptr, &blob)))
      return;
    // shader_<hash>_<mod>.d3d12.bin.<ext> -> shader_<hash>_<mod>.d3d12.<ext>, the name Translation::Dump uses.
    std::filesystem::path out = binary;
    const std::filesystem::path ext = out.extension();
    out.replace_extension();
    out.replace_extension(ext);
    const char* text = static_cast<const char*>(blob->GetBufferPointer());
    std::ofstream(out, std::ios::binary).write(text, std::streamsize(std::strlen(text)));
    blob->Release();
  }

  void Start(Presenter& presenter) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm t{};
    localtime_s(&t, &tt);
    if (!queue) queue = std::make_unique<ImageWriteQueue>();
    session.Start(ShaderCaptureDir(root, game_profile::Get().id, t), std::chrono::steady_clock::now());
    frame_index = 0;
    std::error_code ec;
    std::filesystem::create_directories(session.Dir() / "dbg", ec);
    written_base = queue->Written();
    dropped_base = queue->Dropped() + presenter.CaptureShotsDropped();
    if (overlay) overlay->SetRecording(true);
    LogLine("[gpu] shader capture started: " + session.Dir().string());
  }

  void Stop(Presenter& presenter) {
    session.Stop();
    presenter.FlushCaptureShots();
    queue->Drain();
    if (overlay) overlay->SetRecording(false);
    LogLine("[gpu] shader capture stopped: " + session.Dir().string() + ", " + std::to_string(session.ShadersSaved()) +
            " shaders, " + std::to_string(queue->Written() - written_base) + " shots written, " +
            std::to_string(queue->Dropped() + presenter.CaptureShotsDropped() - dropped_base) + " dropped");
  }

  std::filesystem::path DebugPath(uint32_t frame, const char* ext) const {
    char name[16];
    std::snprintf(name, sizeof name, "%05u", frame);
    return session.Dir() / "dbg" / (std::string(name) + ext);
  }

  // Command processor thread, before the Present of the frame whose draws were noted.
  void OnSwap(Presenter& presenter) {
    if (toggle.exchange(false)) {
      if (session.Active())
        Stop(presenter);
      else
        Start(presenter);
    }
    if (!session.Active()) return;
    const std::vector<DrawRecord> draws = session.TakeFrame();
    // Every frame's list (the shots are 5 per second): a query decides the next frame's draws.
    if (all_frames.is_open() && all_frames_dir != session.Dir()) all_frames.close();
    if (!all_frames.is_open()) {
      all_frames_dir = session.Dir();
      all_frames.open(all_frames_dir / "frames_all.txt", std::ios::binary);
    }
    all_frames << FormatFrameList(0, presenter.FramesPresented() + 1, 0, draws) << std::flush;
    if (!boxes.empty()) std::ofstream(DebugPath(frame_index, ".boxes.txt"), std::ios::binary) << boxes << box_stream_text;
    boxes.clear();
    box_stream_text.clear();
    box_streams.clear();
    ++frame_index;
    const auto due = session.ShotDue(std::chrono::steady_clock::now());
    if (!due) return;
    char name[16];
    std::snprintf(name, sizeof name, "%05u", due->first);
    ImageJob job;
    job.png = session.Dir() / "frames" / (std::string(name) + ".png");
    job.text = session.Dir() / "frames" / (std::string(name) + ".txt");
    job.text_contents = FormatFrameList(due->first, presenter.FramesPresented() + 1, due->second, draws);
    presenter.QueueCaptureShot(std::move(job), queue.get());
  }
};

// Set while a backend with --shader-capture exists (Xenia's hook is a plain function pointer).
std::atomic<ShaderCapture*> shader_capture{nullptr};

void ObserveDraw(xe::gpu::d3d12::D3D12Shader::D3D12Translation& vs, xe::gpu::d3d12::D3D12Shader::D3D12Translation* ps,
                 bool skipped) {
  ShaderCapture* c = shader_capture.load();
  if (!c || !c->session.Active()) return;
  c->session.NoteDraw({vs.shader().ucode_data_hash(), vs.modification(), ps ? ps->shader().ucode_data_hash() : 0,
                       ps ? ps->modification() : 0, skipped, DrawRecord::Kind::kDraw, c->depth_control,
                       c->mode_control, c->color_mask, c->depth_info, c->regs});
  // Only issued draws: on a skipped one the pipeline creation threads may still be finishing the translation
  // (binary replaced, disassembly set after is_translated); a ready pipeline means it is published.
  if (skipped) return;
  c->Dump(vs);
  if (ps) c->Dump(*ps);
}

void ObserveZpd(bool retired, uint64_t handle, uint32_t address, uint64_t z_pass, uint64_t z_fail) {
  ShaderCapture* c = shader_capture.load();
  if (!c || !c->session.Active()) return;
  c->session.NoteDraw({handle, address, z_pass, z_fail, false,
                       retired ? DrawRecord::Kind::kZpdRetired : DrawRecord::Kind::kZpdQueued});
}

// Xenia's D3D12CommandProcessor with gow2's presentation: in Xenia the
// presenter shows the guest output from the UI thread; here the
// command processor thread presents on every XE_SWAP.
class Gow2D3D12CommandProcessor final : public xe::gpu::d3d12::D3D12CommandProcessor {
 public:
  Gow2D3D12CommandProcessor(xe::gpu::d3d12::D3D12GraphicsSystem* graphics_system, Presenter* presenter,
                            std::shared_ptr<LiveGpuSettings> live, std::filesystem::path shader_cache_root,
                            uint32_t title_id)
      : D3D12CommandProcessor(graphics_system, graphics_system->kernel_state()),
        presenter_(presenter),
        live_(std::move(live)),
        antialiasing_(live_->antialiasing),
        shader_cache_root_(std::move(shader_cache_root)),
        title_id_(title_id) {
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
    parts_ = 0;
    ++swaps_;
    std::erase_if(retired_, [&](const auto& e) { return swaps_ - e.first > 3; });
    if (ShaderCapture* c = shader_capture.load(); c && presenter_) {
      ReadQueryDepth(*c);
      c->OnSwap(*presenter_);
    }
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

  // On the command processor thread before the first packet. Non-blocking: the stored shaders are translated
  // here (in parallel), the stored pipelines are queued to the creation threads and the ring starts at once;
  // draws whose pipeline is not created yet are skipped as with a pipeline seen for the first time.
  bool SetupContext() override {
    if (!D3D12CommandProcessor::SetupContext()) return false;
    if (!shader_cache_root_.empty()) {
      const auto start = std::chrono::steady_clock::now();
      InitializeShaderStorage(shader_cache_root_, title_id_, false);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      char line[128];
      std::snprintf(line, sizeof line, "[gpu] shader storage %08X: loaded in %lld ms (pipelines created in the background)",
                    title_id_, static_cast<long long>(ms.count()));
      LogLine(line);
    }
    return true;
  }

  // The last Present may still read the capture textures (released with the command processor): the base
  // waits only for Xenia's work.
  void ShutdownContext() override {
    if (ShaderCapture* c = shader_capture.load(); c && presenter_ && c->session.Active()) c->Stop(*presenter_);
    if (presenter_) presenter_->WaitIdle();
    D3D12CommandProcessor::ShutdownContext();
  }

  // Resolve (copy from EDRAM to guest memory): counted in the summary.
  bool IssueCopy() override {
    kernel::stats::NoteGpuResolve();
    if (ShaderCapture* c = shader_capture.load(); c && c->session.Active()) {
      const uint32_t* v = register_file_->values;
      using namespace xe::gpu;
      DrawRecord d{v[XE_GPU_REG_RB_COPY_CONTROL], v[XE_GPU_REG_RB_COPY_DEST_BASE], v[XE_GPU_REG_RB_COPY_DEST_INFO],
                   v[XE_GPU_REG_RB_COPY_DEST_PITCH], false, DrawRecord::Kind::kCopy};
      d.depth_info = v[XE_GPU_REG_RB_DEPTH_INFO];
      d.color_mask = v[XE_GPU_REG_RB_COLOR_INFO];
      c->session.NoteDraw(d);
    }
    return D3D12CommandProcessor::IssueCopy();
  }

  // Upscaler: the profile's post draws (game_profile::PostCapture; in UE3 the last one before the HUD is the uber
  // post: motion blur, depth of field, tonemapping) give the image without HUD, the depth, the velocity buffer and
  // the reprojection constants.
  bool IssueDraw(xe::gpu::xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    xe::gpu::Shader* ps = active_pixel_shader();  // already this draw's (set by the packets before)
    if (ShaderCapture* c = shader_capture.load(); c && c->session.Active()) {
      c->depth_control = register_file_->values[xe::gpu::XE_GPU_REG_RB_DEPTHCONTROL];
      c->mode_control = register_file_->values[xe::gpu::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      c->color_mask = register_file_->values[xe::gpu::XE_GPU_REG_RB_COLOR_MASK];
      c->depth_info = register_file_->values[xe::gpu::XE_GPU_REG_RB_DEPTH_INFO];
      // regs: fetch constant 0 (6), PA_CL_VPORT_X/Y/Z SCALE/OFFSET (6), RB_SURFACE_INFO, PA_SC_WINDOW_OFFSET,
      // PA_SC_WINDOW_SCISSOR_BR, PA_SU_VTX_CNTL, VS c254-c255 (8), PS c255 (4), PA_CL_VTE_CNTL, PA_SC_AA_CONFIG,
      // RB_COPY_CONTROL, RB_COPY_DEST_INFO.
      using namespace xe::gpu;
      static constexpr uint32_t kFetch0 = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, kVport = XE_GPU_REG_PA_CL_VPORT_XSCALE,
                                kVs254 = XE_GPU_REG_SHADER_CONSTANT_000_X + 254 * 4,
                                kPs255 = XE_GPU_REG_SHADER_CONSTANT_256_X + 255 * 4;
      static constexpr uint32_t kRegs[32] = {
          kFetch0, kFetch0 + 1, kFetch0 + 2, kFetch0 + 3, kFetch0 + 4, kFetch0 + 5,
          kVport, kVport + 1, kVport + 2, kVport + 3, kVport + 4, kVport + 5,
          XE_GPU_REG_RB_SURFACE_INFO, XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
          XE_GPU_REG_PA_SU_VTX_CNTL,
          kVs254, kVs254 + 1, kVs254 + 2, kVs254 + 3, kVs254 + 4, kVs254 + 5, kVs254 + 6, kVs254 + 7,
          kPs255, kPs255 + 1, kPs255 + 2, kPs255 + 3,
          XE_GPU_REG_PA_CL_VTE_CNTL, XE_GPU_REG_PA_SC_AA_CONFIG, XE_GPU_REG_RB_COPY_CONTROL,
          XE_GPU_REG_RB_COPY_DEST_INFO};
      for (size_t r = 0; r < c->regs.size(); ++r) c->regs[r] = register_file_->values[kRegs[r]];
    }
    const game_profile::PostCapture& post = game_profile::Get().post;
    const uint64_t hash = (post.final_ps || post.motion_ps) && ps ? ps->ucode_data_hash() : 0;
    // [patches] disable_motion_blur: velocity scales of the motion draw to 0 only for this draw (GoW2: c17.xy camera,
    // c18.xy dynamic objects); restored immediately, the capture reads the real ones.
    uint32_t saved[4];
    const bool zero_blur = hash && hash == post.motion_ps && live_->no_motion_blur;
    if (zero_blur)
      for (int i = 0; i < 4; ++i) {
        saved[i] = register_file_->values[kPsConst + post.blur_off[i]];
        WriteRegister(kPsConst + post.blur_off[i], 0);
      }
    const bool ok = D3D12CommandProcessor::IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit);
    if (ShaderCapture* c = shader_capture.load(); c && c->session.Active()) {
      NoteQueryBox(*c, index_count, index_buffer_info);
      if (ok && c->DebugFrame() && ps && ps->writes_depth()) CaptureQueryDepth(*c);
    }
    if (zero_blur)
      for (int i = 0; i < 4; ++i) WriteRegister(kPsConst + post.blur_off[i], saved[i]);
    if (!ok || !hash || !presenter_ || !presenter_->CaptureWanted()) return ok;
    // A final draw with a 1x1 size texture is not a view (GoW2's Tac-Com adds a second uber post with 1x1 depth): it
    // is not captured, nor the parts of the same draw (otherwise 2 captures = frame without upscaler, and without
    // OptiScaler's FG). The depth and motion parts are taken once per frame.
    const auto size = register_file_->GetTextureFetch(post.size_tf).size_2d;
    if (hash == post.final_ps && !size.width && !size.height) return ok;
    if (hash == post.depth_ps && !(parts_ & kDepthPart)) CaptureDepth(post);
    if (hash == post.motion_ps && !(parts_ & kMotionPart)) CaptureMotion(post);
    if (hash == post.final_ps) CaptureFinal(post);
    return ok;
  }

 private:
  static constexpr uint32_t kPsConst = xe::gpu::XE_GPU_REG_SHADER_CONSTANT_256_X;  // pixel shader c0

  // Shader capture: on a debug frame, the k_24_8_FLOAT texture of fetch constant 0 of each depth-writing draw (up to
  // kDepthSlots; GoW3 restores the scene depth after the shadow maps, then builds the half-res occlusion query depth,
  // both from resolves to the same address), copied as the GPU sees it (scaled with the internal resolution) into a
  // readback buffer. ReadQueryDepth writes them at the next swap as dbg/<frame>.depth<k>.bin (rows of row_pitch bytes)
  // + .depth<k>.txt "width height row_pitch dxgi_format scale_x scale_y rb_depth_info" (RB_DEPTH_INFO of the draw: the
  // depth buffer it wrote, to match the query boxes' "di").
  void CaptureQueryDepth(ShaderCapture& c) {
    if (depth_slots_used_ >= kDepthSlots) return;
    const auto t = register_file_->GetTextureFetch(0);
    if (t.format != xe::gpu::xenos::TextureFormat::k_24_8_FLOAT) return;
    auto& tc = Gow2TextureCache();
    const auto& rtc = Gow2RenderTargetCache();
    ID3D12Resource* src = tc.Gow2ActiveTexture(0);
    if (!src) return;
    DepthSlot& s = depth_slots_[depth_slots_used_];
    const uint32_t sx = rtc.GetDrawScaleX(), sy = rtc.GetDrawScaleY();
    const uint32_t w = CopySizeOfGuestTexture(t.size_2d.width + 1, sx, src->GetDesc().Width);
    const uint32_t h = CopySizeOfGuestTexture(t.size_2d.height + 1, sy, src->GetDesc().Height);
    if (!EnsureCaptureTarget(s.texture, w, h, src->GetDesc().Format) || !tc.Gow2CopyActiveTexture(0, s.texture.Get(), w, h))
      return;
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    const D3D12_RESOURCE_DESC desc = s.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!s.readback || s.readback->GetDesc().Width < total) {
      D3D12_RESOURCE_DESC b{};
      b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      b.Width = total;
      b.Height = 1;
      b.DepthOrArraySize = 1;
      b.MipLevels = 1;
      b.SampleDesc.Count = 1;
      b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      s.readback.Reset();
      if (FAILED(device->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &b,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback))))
        return;
    }
    PushTransitionBarrier(s.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = s.texture.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = s.readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = fp;
    GetDeferredCommandList().D3DCopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    PushTransitionBarrier(s.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    s.depth_info = register_file_->values[xe::gpu::XE_GPU_REG_RB_DEPTH_INFO];
    s.width = w;
    s.height = h;
    s.row_pitch = fp.Footprint.RowPitch;
    s.format = uint32_t(desc.Format);
    s.scale_x = sx;
    s.scale_y = sy;
    depth_slots_frame_ = c.frame_index;
    ++depth_slots_used_;
  }

  // Guest memory [address, address + size) as the GPU has it in Xenia's shared memory right after the draw that read it
  // (uploaded for that draw), into gpu_streams_readback_; ReadQueryDepth writes dbg/<frame>.gpustreams.bin and .txt
  // ("address offset size" per stream, raw guest bytes).
  void CopyGpuStream(ShaderCapture& c, uint32_t address, uint32_t size) {
    if (gpu_streams_used_ + size > kGpuStreamBytes) return;
    if (!gpu_streams_readback_) {
      D3D12_RESOURCE_DESC b{};
      b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      b.Width = kGpuStreamBytes;
      b.Height = 1;
      b.DepthOrArraySize = 1;
      b.MipLevels = 1;
      b.SampleDesc.Count = 1;
      b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(GetD3D12Provider().GetDevice()->CreateCommittedResource(
              &xe::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&gpu_streams_readback_))))
        return;
    }
    auto& shared = Gow2SharedMemory();
    shared.UseAsCopySource();
    SubmitBarriers();
    GetDeferredCommandList().D3DCopyBufferRegion(gpu_streams_readback_.Get(), gpu_streams_used_, shared.GetBuffer(),
                                                 address, size);
    gpu_streams_.push_back({address, gpu_streams_used_, size});
    gpu_streams_used_ += size;
    depth_slots_frame_ = c.frame_index;
  }

  // Depth plane of the bound depth render target (host, scaled, whole EDRAM-sized resource) into target_readback_; read
  // with the depth copies at the next swap: dbg/<frame>.qdepth.bin + .qdepth.txt "width height row_pitch dxgi_format".
  void CaptureQueryTarget() {
    auto& rtc = Gow2RenderTargetCache();
    uint64_t need = 0;
    if (!rtc.Gow2CopyDepthTarget(target_readback_.Get(), target_readback_size_, target_footprint_, need)) {
      if (!need || need <= target_readback_size_) return;
      D3D12_RESOURCE_DESC b{};
      b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      b.Width = need;
      b.Height = 1;
      b.DepthOrArraySize = 1;
      b.MipLevels = 1;
      b.SampleDesc.Count = 1;
      b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      target_readback_.Reset();
      target_readback_size_ = 0;
      if (FAILED(GetD3D12Provider().GetDevice()->CreateCommittedResource(
              &xe::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&target_readback_))))
        return;
      target_readback_size_ = need;
      if (!rtc.Gow2CopyDepthTarget(target_readback_.Get(), target_readback_size_, target_footprint_, need)) return;
    }
    target_taken_ = true;
  }

  // At the swap after the copies (their submission has ended): waits for the GPU (debug only) and writes the files.
  void ReadQueryDepth(ShaderCapture& c) {
    if ((!depth_slots_used_ && gpu_streams_.empty()) || c.frame_index == depth_slots_frame_) return;
    const uint32_t used = depth_slots_used_;
    const bool target = target_taken_;
    const std::vector<GpuStream> streams = std::move(gpu_streams_);
    depth_slots_used_ = 0;
    target_taken_ = false;
    gpu_streams_.clear();
    gpu_streams_used_ = 0;
    if (!c.session.Active()) return;
    // Everything submitted so far (the copies' submission ended at the swap that recorded them) done: own fence after it.
    if (!query_depth_fence_ && FAILED(GetD3D12Provider().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                                                  IID_PPV_ARGS(&query_depth_fence_))))
      return;
    if (FAILED(GetD3D12Provider().GetDirectQueue()->Signal(query_depth_fence_.Get(), ++query_depth_fence_value_)))
      return;
    if (query_depth_fence_->GetCompletedValue() < query_depth_fence_value_)
      query_depth_fence_->SetEventOnCompletion(query_depth_fence_value_, nullptr);  // blocks
    if (!streams.empty()) {
      const uint32_t bytes = streams.back().offset + streams.back().size;
      void* mapped = nullptr;
      const D3D12_RANGE range{0, bytes};
      if (SUCCEEDED(gpu_streams_readback_->Map(0, &range, &mapped))) {
        std::ofstream(c.DebugPath(depth_slots_frame_, ".gpustreams.bin"), std::ios::binary)
            .write(static_cast<const char*>(mapped), std::streamsize(bytes));
        const D3D12_RANGE none{0, 0};
        gpu_streams_readback_->Unmap(0, &none);
        std::string index;
        char buf[64];
        for (const GpuStream& g : streams) {
          std::snprintf(buf, sizeof buf, "%08X %u %u\n", g.address, g.offset, g.size);
          index += buf;
        }
        std::ofstream(c.DebugPath(depth_slots_frame_, ".gpustreams.txt"), std::ios::binary) << index;
      }
    }
    if (target) {
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = target_footprint_;
      void* mapped = nullptr;
      const D3D12_RANGE range{0, size_t(fp.Footprint.RowPitch) * fp.Footprint.Height};
      if (SUCCEEDED(target_readback_->Map(0, &range, &mapped))) {
        std::ofstream(c.DebugPath(depth_slots_frame_, ".qdepth.bin"), std::ios::binary)
            .write(static_cast<const char*>(mapped), std::streamsize(range.End));
        const D3D12_RANGE none{0, 0};
        target_readback_->Unmap(0, &none);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%u %u %u %u\n", fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch,
                      uint32_t(fp.Footprint.Format));
        std::ofstream(c.DebugPath(depth_slots_frame_, ".qdepth.txt"), std::ios::binary) << buf;
      }
    }
    for (uint32_t k = 0; k < used; ++k) {
      const DepthSlot& d = depth_slots_[k];
      void* mapped = nullptr;
      const D3D12_RANGE range{0, size_t(d.row_pitch) * d.height};
      if (FAILED(d.readback->Map(0, &range, &mapped))) continue;
      const std::string ext = ".depth" + std::to_string(k);
      std::ofstream(c.DebugPath(depth_slots_frame_, (ext + ".bin").c_str()), std::ios::binary)
          .write(static_cast<const char*>(mapped), std::streamsize(range.End));
      const D3D12_RANGE none{0, 0};
      d.readback->Unmap(0, &none);
      char buf[128];
      std::snprintf(buf, sizeof buf, "%u %u %u %u %u %u %08X\n", d.width, d.height, d.row_pitch, d.format, d.scale_x,
                    d.scale_y, d.depth_info);
      std::ofstream(c.DebugPath(depth_slots_frame_, (ext + ".txt").c_str()), std::ios::binary) << buf;
    }
  }

  // Shader capture: a draw that only tests depth (UE3's occlusion query boxes) is written to the frame's boxes text
  // with what it takes to redo its vertex shader offline (dbg/<frame>.boxes.txt):
  // "box <vs hash> prim <type> off <VGT_INDX_OFFSET> idx <indices...>", "  c <VS c0-c3, c254, c255 as hex>",
  // "  vf <fetch constant> <stride words> <guest address> <endian>"; then once per frame and address
  // "stream <address> <endian> <bytes as hex>" (at most 16 KB from the address, as in guest memory).
  void NoteQueryBox(ShaderCapture& c, uint32_t index_count, const IndexBufferInfo* ib) {
    using namespace xe::gpu;
    const reg::RB_DEPTHCONTROL dc = register_file_->Get<reg::RB_DEPTHCONTROL>();
    xe::gpu::Shader* vs = active_vertex_shader();
    if (!c.DebugFrame() || !vs || !dc.z_enable || dc.z_write_enable ||
        register_file_->values[XE_GPU_REG_RB_COLOR_MASK])
      return;
    // The first query box on the depth buffer the last depth copy's draw wrote: that depth buffer as the boxes test it.
    if (depth_slots_used_ && !target_taken_ &&
        register_file_->values[XE_GPU_REG_RB_DEPTH_INFO] == depth_slots_[depth_slots_used_ - 1].depth_info)
      CaptureQueryTarget();
    char buf[64];
    std::string& t = c.boxes;
    std::snprintf(buf, sizeof buf, "box %016llX prim %u off %u idx", static_cast<unsigned long long>(vs->ucode_data_hash()),
                  register_file_->values[XE_GPU_REG_VGT_DRAW_INITIATOR] & 0x3F,
                  register_file_->values[XE_GPU_REG_VGT_INDX_OFFSET]);
    t += buf;
    for (uint32_t i = 0; i < std::min(index_count, 4096u); ++i) {
      uint32_t v = i;
      if (ib && ib->guest_base) {
        const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(ib->guest_base);
        v = ib->format == xenos::IndexFormat::kInt32
                ? (uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3])
                : (uint32_t(p[i * 2]) << 8 | p[i * 2 + 1]);
      }
      std::snprintf(buf, sizeof buf, " %u", v);
      t += buf;
    }
    t += "\n  c";
    for (uint32_t r : {0u, 1u, 2u, 3u, 254u, 255u})
      for (uint32_t k = 0; k < 4; ++k) {
        std::snprintf(buf, sizeof buf, " %08X", register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X + r * 4 + k]);
        t += buf;
      }
    t += '\n';
    for (const Shader::VertexBinding& b : vs->vertex_bindings()) {
      const xenos::xe_gpu_vertex_fetch_t f = register_file_->GetVertexFetch(b.fetch_constant);
      const uint32_t address = f.address << 2;
      std::snprintf(buf, sizeof buf, "  vf %u %u %08X %u\n", b.fetch_constant, b.stride_words, address,
                    uint32_t(f.endian));
      t += buf;
      if (!c.box_streams.insert(address).second) continue;
      CopyGpuStream(c, address, std::min<uint32_t>(f.size * 4, 16384));
      std::string s = "stream ";
      std::snprintf(buf, sizeof buf, "%08X %u ", address, uint32_t(f.endian));
      s += buf;
      const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(address);
      for (uint32_t i = 0; i < std::min<uint32_t>(f.size * 4, 16384); ++i) {
        std::snprintf(buf, sizeof buf, "%02X", p[i]);
        s += buf;
      }
      c.box_stream_text += s + '\n';
    }
  }
  static constexpr uint32_t kDepthPart = 1, kMotionPart = 2;

  // Pixel shader constant c<c> of the register file.
  void ReadConstant(uint32_t c, float* out) const {
    for (uint32_t k = 0; k < 4; ++k) out[k] = register_file_->Get<float>(kPsConst + c * 4 + k);
  }

  // Frame without upscaler; the reason in the log for the first failure and again for the first one after the first
  // successful capture (GoW3's menus fail harmlessly before gameplay and would otherwise hide a later real failure).
  void CaptureFailed() {
    if (!capture_failed_logged_)
      LogLine("[gpu] upscaler: capture after the uber post failed (ROV path or textures missing), frame without upscaler");
    capture_failed_logged_ = true;
  }

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

  // Scene depth at internal resolution and its linearization constant.
  void CaptureDepth(const game_profile::PostCapture& post) {
    auto& tc = Gow2TextureCache();
    const auto& rtc = Gow2RenderTargetCache();
    ID3D12Resource* depth = tc.Gow2ActiveTexture(post.depth_tf);
    const auto t = register_file_->GetTextureFetch(post.depth_tf);
    const uint32_t w = (t.size_2d.width + 1) * rtc.GetDrawScaleX(), h = (t.size_2d.height + 1) * rtc.GetDrawScaleY();
    if (!depth || !EnsureCaptureTarget(capture_.depth, w, h, depth->GetDesc().Format) ||
        !tc.Gow2CopyActiveTexture(post.depth_tf, capture_.depth.Get(), w, h))
      return;
    ReadConstant(post.linearize_c, capture_.k.c2);
    parts_ |= kDepthPart;
  }

  // Velocity buffer, reprojection and velocity scale.
  void CaptureMotion(const game_profile::PostCapture& post) {
    auto& tc = Gow2TextureCache();
    const auto& rtc = Gow2RenderTargetCache();
    ID3D12Resource* velocity = tc.Gow2ActiveTexture(post.velocity_tf);
    const auto t = register_file_->GetTextureFetch(post.velocity_tf);
    // The velocity buffer (640 px) stays native with draw_resolution_scale_threshold: copied as is.
    const uint32_t sx = rtc.GetDrawScaleX(), sy = rtc.GetDrawScaleY();
    const uint32_t vw = velocity ? CopySizeOfGuestTexture(t.size_2d.width + 1, sx, velocity->GetDesc().Width) : 0;
    const uint32_t vh = velocity ? CopySizeOfGuestTexture(t.size_2d.height + 1, sy, velocity->GetDesc().Height) : 0;
    if (!velocity || !EnsureCaptureTarget(capture_.velocity, vw, vh, velocity->GetDesc().Format) ||
        !tc.Gow2CopyActiveTexture(post.velocity_tf, capture_.velocity.Get(), vw, vh))
      return;
    UberPostConstants& k = capture_.k;
    ReadConstant(post.reprojection_c, k.c13);
    ReadConstant(post.reprojection_c + 1, k.c14);
    ReadConstant(post.reprojection_c + 2, k.c15);
    ReadConstant(post.reprojection_c + 3, k.c16);
    ReadConstant(post.blur_scale_c, k.c17);
    k.c17[2] = k.c17[3] = 1.0f;
    if (post.blur_limit_c != game_profile::PostCapture::kNone) {
      float limit[4];
      ReadConstant(post.blur_limit_c, limit);
      k.c17[2] = limit[2];
      k.c17[3] = limit[3];
    }
    // The SRV takes the guest components after the fetch swizzle (Xenia's host swizzle of the binding).
    k.velocity_mapping = post.velocity_fetch_swizzle
                             ? tc.GetActiveTextureHostSwizzle(post.velocity_tf) |
                                   D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES
                             : 0;
    parts_ |= kMotionPart;
  }

  // Final draw: image without HUD (render target 0) at the size of `size_tf`; the frame's capture is complete if
  // the depth and motion parts came before it.
  void CaptureFinal(const game_profile::PostCapture& post) {
    auto& rtc = Gow2RenderTargetCache();
    ID3D12Resource* color = rtc.Gow2ColorTarget0();
    const auto t = register_file_->GetTextureFetch(post.size_tf);
    const uint32_t w = (t.size_2d.width + 1) * rtc.GetDrawScaleX(), h = (t.size_2d.height + 1) * rtc.GetDrawScaleY();
    const bool ok = parts_ == (kDepthPart | kMotionPart) && color &&
                    EnsureCaptureTarget(capture_.prehud, w, h, color->GetDesc().Format) &&
                    rtc.Gow2CopyColorTarget0(capture_.prehud.Get(), w, h);
    if (!ok) return CaptureFailed();
    if (presenter_->HdrActive() && post.hdr) CaptureHdr(post);
    // Like Xenia's IssueSwap: PWL for a 2_10_10_10 front buffer, table otherwise (here from the format of the
    // uber post's render target, which is what ends up in the front buffer).
    const DXGI_FORMAT f = color->GetDesc().Format;
    capture_.ramp.pwl = f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R10G10B10A2_UNORM;
    static_assert(sizeof(xe::gpu::reg::DC_LUT_30_COLOR) == 4 && sizeof(xe::gpu::reg::DC_LUT_PWL_DATA) == 4);
    std::memcpy(capture_.ramp.table, gamma_ramp_256_entry_table(), sizeof(capture_.ramp.table));
    std::memcpy(capture_.ramp.pwl_rgb, gamma_ramp_pwl_rgb(), sizeof(capture_.ramp.pwl_rgb));
    scene_pitch_ = t.size_2d.width + 1;  // guest width of the scene: pitch of its surfaces
    ++captures_;
    if (!captured_once_) capture_failed_logged_ = false;  // re-arm the failure line once
    captured_once_ = true;
  }

  // HDR10 output: scene before tonemapping and bloom, uber post constants c0..c18 and c253..c255 (the layout of
  // gpu/hdr_passes.h), filter and exp_adjust of scene, bloom and velocity. Failed copies: empty scene/bloom, frame
  // without residual.
  void CaptureHdr(const game_profile::PostCapture& post) {
    auto& tc = Gow2TextureCache();
    const auto& rf = *register_file_;
    HdrUberConstants& k = capture_.hdr;
    for (uint32_t i = 0; i < 19; ++i) ReadConstant(i, k.c[i]);
    ReadConstant(253, k.c253);
    ReadConstant(254, k.c254);
    ReadConstant(255, k.c255);
    const uint8_t slots[3] = {post.hdr_scene_tf, post.hdr_bloom_tf, post.velocity_tf};
    for (uint32_t i = 0; i < 3; ++i) {
      const auto f = rf.GetTextureFetch(slots[i]);
      k.linear[i] = f.mag_filter == xe::gpu::xenos::TextureFilter::kLinear;
      k.exp_scale[i] = std::ldexp(1.0f, f.exp_adjust);
    }
    const uint32_t sx = Gow2RenderTargetCache().GetDrawScaleX(), sy = Gow2RenderTargetCache().GetDrawScaleY();
    ID3D12Resource* scene = tc.Gow2ActiveTexture(post.hdr_scene_tf);
    ID3D12Resource* bloom = tc.Gow2ActiveTexture(post.hdr_bloom_tf);
    const auto t1 = rf.GetTextureFetch(post.hdr_scene_tf), t2 = rf.GetTextureFetch(post.hdr_bloom_tf);
    const uint32_t w1 = (t1.size_2d.width + 1) * sx, h1 = (t1.size_2d.height + 1) * sy;
    const uint32_t w2 = (t2.size_2d.width + 1) * sx, h2 = (t2.size_2d.height + 1) * sy;
    const bool ok = scene && bloom && EnsureCaptureTarget(capture_.scene, w1, h1, scene->GetDesc().Format) &&
                    EnsureCaptureTarget(capture_.bloom, w2, h2, bloom->GetDesc().Format) &&
                    tc.Gow2CopyActiveTexture(post.hdr_scene_tf, capture_.scene.Get(), w1, h1) &&
                    tc.Gow2CopyActiveTexture(post.hdr_bloom_tf, capture_.bloom.Get(), w2, h2);
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
  std::filesystem::path shader_cache_root_;  // D3D12Options::shader_cache_root
  uint32_t title_id_;
  UpscaleCapture capture_{};
  uint32_t captures_ = 0;  // uber posts captured in the current frame (capture_ is the last one)
  uint32_t scene_pitch_ = 0;  // scene surfaces for the jitter (0 = none until there is a capture)
  uint32_t parts_ = 0;  // k*Part captured in the current frame
  bool capture_failed_logged_ = false;
  bool captured_once_ = false;
  bool multi_capture_logged_ = false;
  bool hdr_capture_failed_logged_ = false;
  uint64_t swaps_ = 0;
  // Shader capture: copies of the depth textures depth-writing draws sampled on a debug frame (CaptureQueryDepth), read
  // back at the next swap.
  static constexpr uint32_t kDepthSlots = 4;
  struct DepthSlot {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture, readback;
    uint32_t depth_info = 0, width = 0, height = 0, row_pitch = 0, format = 0, scale_x = 1, scale_y = 1;
  };
  std::array<DepthSlot, kDepthSlots> depth_slots_;
  uint32_t depth_slots_used_ = 0, depth_slots_frame_ = 0;
  // Vertex streams of the query boxes as the GPU read them (shared memory, CopyGpuStream): dbg/<frame>.gpustreams.bin.
  static constexpr uint32_t kGpuStreamBytes = 4u << 20;
  struct GpuStream {
    uint32_t address, offset, size;
  };
  Microsoft::WRL::ComPtr<ID3D12Resource> gpu_streams_readback_;
  std::vector<GpuStream> gpu_streams_;
  uint32_t gpu_streams_used_ = 0;
  // The depth render target the query boxes test (CaptureQueryTarget): dbg/<frame>.qdepth.bin.
  Microsoft::WRL::ComPtr<ID3D12Resource> target_readback_;
  uint64_t target_readback_size_ = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_footprint_{};
  bool target_taken_ = false;
  Microsoft::WRL::ComPtr<ID3D12Fence> query_depth_fence_;
  uint64_t query_depth_fence_value_ = 0;
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;  // (swap, resource)
};

void ApplyBuiltinOverrides(xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov, bool bindless) {
  ApplyOverride(BuiltinShaderOverrides(), t, rov, bindless);
}

class D3D12Backend final : public Backend {
 public:
  D3D12Backend(std::unique_ptr<xe::Memory> memory, std::unique_ptr<xe::ui::d3d12::D3D12Provider> provider,
               std::unique_ptr<Presenter> presenter, std::shared_ptr<LiveGpuSettings> live,
               std::filesystem::path shader_cache_root, uint32_t title_id)
      : memory_(std::move(memory)), provider_(std::move(provider)), presenter_(std::move(presenter)) {
    Presenter* p = presenter_.get();
    auto graphics_system =
        std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>(memory_.get(), provider_.get(), p);
    inner_ = CreateXeniaBackend(std::move(graphics_system),
                                [p, live, root = std::move(shader_cache_root),
                                 title_id](xe::gpu::GraphicsSystem* gs) -> std::unique_ptr<xe::gpu::CommandProcessor> {
                                  using Effect = xe::gpu::CommandProcessor::SwapPostEffect;
                                  static_assert(int(Effect::kNone) == 0 && int(Effect::kFxaa) == 1 &&
                                                int(Effect::kFxaaExtreme) == 2);
                                  return std::make_unique<Gow2D3D12CommandProcessor>(
                                      static_cast<xe::gpu::d3d12::D3D12GraphicsSystem*>(gs), p, live, root, title_id);
                                });
  }

  // First the command processor (thread stopped, then GraphicsSystem and CP
  // destroyed: it uses the presenter, device and xe::Memory callbacks), then
  // presenter, device and xe::Memory (removes the callbacks left over by physwatch).
  ~D3D12Backend() override {
    inner_.reset();
    xe::gpu::d3d12::SetTranslatedPixelShaderHook(nullptr);  // global: does not stay for the following backends
    if (capture_) {
      xe::gpu::d3d12::SetDrawObserverHook(nullptr);
      xe::gpu::SetZPDObserverHook(nullptr);
      shader_capture.store(nullptr);
      capture_.reset();
    }
    presenter_.reset();
    provider_.reset();
    memory_.reset();
  }

  bool valid() const { return inner_ != nullptr; }

  // --shader-capture: installs the draw observer; the capture lives as long as the backend.
  void SetShaderCapture(std::unique_ptr<ShaderCapture> capture) {
    capture_ = std::move(capture);
    capture_->provider = provider_.get();
    shader_capture.store(capture_.get());
    xe::gpu::d3d12::SetDrawObserverHook(&ObserveDraw);
    xe::gpu::SetZPDObserverHook(&ObserveZpd);
  }

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
  std::unique_ptr<ShaderCapture> capture_;
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
  live->no_motion_blur = options.settings && options.settings->patches.contains("disable_motion_blur") &&
                         game_patches::Available(game_patches::kDisableMotionBlur);
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
      } else if (!game_profile::Get().post.hdr) {
        LogLine("[hdr] HDR not available in this game (no reconstruction of its post processing): SDR");
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
  std::unique_ptr<ShaderCapture> capture;
  if (options.shader_capture && presenter) {
    capture = std::make_unique<ShaderCapture>();
    capture->root = options.config.parent_path();
  }
  if (presenter && options.settings) {
    Presenter* p = presenter.get();
    auto overlay = Overlay::Create(
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
          // Motion blur through the motion draw's constants in a game with a post capture; otherwise its hooks.
          const bool blur_by_constants = game_profile::Get().post.motion_ps != 0;
          for (const auto& [key, id] : {std::pair{"show_fps", PatchId::kShowFps},
                                        std::pair{"disable_motion_blur", PatchId::kDisableMotionBlur},
                                        std::pair{"disable_lens_flares", PatchId::kDisableLensFlares},
                                        std::pair{"disable_bloom", PatchId::kDisableBloom}})
            if ((id != PatchId::kDisableMotionBlur || !blur_by_constants) &&
                s.patches.contains(key) != run.patches.contains(key))
              game_patches::SetEnabled(mem, id, s.patches.contains(key));
          if (const bool on = s.patches.contains("disable_motion_blur");
              blur_by_constants && game_patches::Available(game_patches::kDisableMotionBlur) &&
              on != run.patches.contains("disable_motion_blur"))
          {
            live->no_motion_blur = on;
            LogLine(std::string("[patch] disable_motion_blur: ") + (on ? "turned on hot" : "turned off hot"));
          }
          const settings::Video &a = run.video, &b = s.video;
          if (!upscaler && a.antialiasing != b.antialiasing) live->antialiasing = uint32_t(b.antialiasing);
          run.patches = s.patches;
          run.video = s.video;
        },
        // F12: screenshot of the next frame in <exe folder>/screenshots/<game id>/.
        [p, dir = options.config.parent_path()] {
          const auto now = std::chrono::system_clock::now();
          const std::time_t tt = std::chrono::system_clock::to_time_t(now);
          std::tm t{};
          localtime_s(&t, &tt);
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
          const std::filesystem::path file = HotkeyScreenshotPath(dir, game_profile::Get().id, t, unsigned(ms % 1000));
          std::error_code ec;
          std::filesystem::create_directories(file.parent_path(), ec);
          p->ArmScreenshot(std::chrono::steady_clock::now(), file);
        },
        // F11 (--shader-capture only): start/stop at the next swap.
        capture ? Overlay::ToggleFn([c = capture.get()] { c->toggle.store(true); }) : Overlay::ToggleFn());
    if (capture) capture->overlay = overlay.get();
    presenter->SetOverlay(std::move(overlay));
  }
  if (capture && !capture->overlay) {
    LogLine("[gpu] --shader-capture without the F1 overlay (no settings): F11 not available");
    capture.reset();
  }
  if (upscaler) {
    const std::string& d = cvars::gow2_upscaler_debug;
    const UpscaleDebug debug = d == "ffx_debug_view" ? UpscaleDebug::kFfxDebugView : UpscaleDebug::kNone;
    presenter->SetUpscaler(std::move(factory), options.sharpening ? options.sharpness : -1.0f, debug);
    if (optiscaler::Loaded(launcher::ExeDir()) &&
        optiscaler::Detect(launcher::ExeDir()) != optiscaler::State::kForeign) {
      // The wait costs ~25% fps (no CPU/GPU overlap): only for NR on at startup; F6 later does not change it.
      std::ifstream f(launcher::ExeDir() / "OptiScaler.ini", std::ios::binary);
      const std::string ini((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (optiscaler::NrEnabled(ini)) {
        presenter->SetWaitGpuBeforePresent(true);
        LogLine("[gpu] OptiScaler NR on: each frame presented after the GPU finishes it (its NR skips unfinished ones)");
      } else {
        LogLine("[gpu] OptiScaler NR off in OptiScaler.ini: no wait before Present");
      }
    }
  }
  // Before the command processor, which creates the pipeline cache. With false the hook
  // is not touched (in the game it stays nullptr; tests may have set their own).
  if (options.enhanced_shaders) xe::gpu::d3d12::SetTranslatedPixelShaderHook(&ApplyBuiltinOverrides);
  const bool frame_generation = presenter && presenter->FrameGenerationActive();
  const bool hdr10 = presenter && presenter->HdrActive();
  auto backend = std::make_unique<D3D12Backend>(std::make_unique<xe::Memory>(mem.base), std::move(provider),
                                                std::move(presenter), live, options.shader_cache_root,
                                                options.title_id);
  if (!backend->valid()) {
    LogLine("[gpu] cannot create Xenia's D3D12 command processor");
    return nullptr;
  }
  if (capture) {
    backend->SetShaderCapture(std::move(capture));
    LogLine("[gpu] shader capture available: F11 starts/stops it");
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

void StopShaderCaptureBeforeExit() {
  ShaderCapture* c = shader_capture.load();
  if (!c || !c->session.Active()) return;
  // Not ShaderCapture::Stop: the presenter belongs to the command processor thread, still running.
  c->session.Stop();
  if (c->queue) c->queue->Drain();
  LogLine("[gpu] shader capture stopped at exit: " + c->session.Dir().string() + ", " +
          std::to_string(c->session.ShadersSaved()) + " shaders, " +
          std::to_string(c->queue ? c->queue->Written() - c->written_base : 0) + " shots written");
}

bool SetXeniaCvar(std::string_view name, std::string_view value) { return gow2::xenia::SetCvar(name, value); }

}  // namespace gpu
