// gow2 d3d12 backend (--gpu d3d12, default): D3D12 device (Xenia's D3D12Provider),
// Xenia's D3D12CommandProcessor running the game's ring
// (gpu::CreateXeniaBackend, register/ring bridge of the GraphicsSystem shim),
// DXGI presenter on the game window (gpu/presenter.h), screenshots.
//
//  - On every XE_SWAP (VdSwap) the command processor thread updates
//    the guest output (Xenia's IssueSwap) and calls Presenter::Present(), even
//    if the front buffer is not available (re-presents the last output):
//    host_frames_presented = XE_SWAPs executed. Failed Present ->
//    xe::FatalError (summary, exit 5).
//  - IssueCopy (resolve) counts gpu_resolves; draws are counted by the gow2 patches.
//  - Physical memory: xe::Memory on the arena (callbacks and observation of the
//    pages in runtime/physical_watch).
//  - Device lost in the command processor: [gpu] device removed reason=...,
//    xe::FatalError (GraphicsSystem::OnHostGpuLossFromAnyThread).
//  - Destruction: command processor (thread stopped), presenter, device,
//    xe::Memory. It must be destroyed (gpu::Shutdown) before the GuestMemory arena.
//
// Header without Xenia or D3D12: main.cpp includes it too.
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "gpu/gpu_system.h"
#include "gpu/screenshot.h"
#include "settings.h"

struct GuestMemory;
namespace plat {
struct Window;
}

namespace gpu {

struct D3D12Options {
  plat::Window* window = nullptr;  // nullptr: no presenter (tests)
  std::vector<ScreenshotRequest> screenshots;    // --screenshot repeatable, queued in the presenter
  std::chrono::steady_clock::time_point start;  // each screenshot is at start + seconds
  bool shader_capture = false;  // --shader-capture (hidden): F11 records shaders and shots (gpu/shader_capture.h)
  // gpu::PresentOptions (presenter.h, which includes Xenia): the back buffer always
  // follows the client area (a fixed back buffer stretched by DXGI on a monitor
  // of a different aspect distorts the image); stretch (false = fit); vsync.
  bool stretch = false;
  bool vsync = true;
  // DXGI exclusive fullscreen, wanted when the window is in
  // fullscreen mode (not windowed via Alt-Enter) and has focus; video mode closest
  // to mode_width x mode_height (0 = the monitor's current mode).
  bool exclusive_fullscreen = false;
  uint32_t mode_width = 0, mode_height = 0;
  // Post-processing. antialiasing: 0 off, 1 fxaa, 2 fxaa_extreme
  // (xe::gpu::CommandProcessor::SwapPostEffect); sharpening/sharpness: CAS.
  uint32_t antialiasing = 0;
  bool sharpening = false;
  float sharpness = 0.0f;
  // Replacement shaders by hash (shader_overrides.h): hook of Xenia's pipeline
  // cache, set only if active.
  bool enhanced_shaders = true;
  // Temporal upscaler (gpu/upscaler.h): 0 off, 1 fsr, 2 dlss (settings::Upscaler).
  uint32_t upscaler = 0;
  settings::DlssPreset dlss_preset = settings::DlssPreset::kDefault;  // DLSS SR preset (upscaler 2 only)
  // Effective frame rate (k30 even after main.cpp's fallback) and VRR: gpu::PresentOptions.
  settings::FrameRate frame_rate = settings::FrameRate::k30;
  settings::Vrr vrr = settings::Vrr::kAuto;
  settings::FrameGeneration frame_generation = settings::FrameGeneration::kOff;  // [video] frame_generation
  // HDR10 output (gpu::PresentOptions::hdr): hdr = [video] hdr or --hdr force; hdr_force also on an SDR output;
  // levels 0 = auto (hdr::ChooseLevels).
  bool hdr = false, hdr_force = false;
  uint32_t hdr_peak_nits = 0, hdr_paper_white_nits = 0;
  // F1 overlay (gpu/overlay.h): settings in force and the file Apply saves them to; nullopt or
  // without a window: no overlay. Apply -> Presenter::SetVideo + plat::ApplyWindowOptions.
  std::optional<settings::Settings> settings;
  std::filesystem::path config;
  // Xenia's persistent shader/pipeline storage in <shader_cache_root>/shaders/shareable/<title id>.*
  // (the per-game user data folder); empty: no storage (tests).
  std::filesystem::path shader_cache_root;
  uint32_t title_id = 0;
};

// nullptr + message in the log if the device cannot be created (main: exit 5,
// suggests --gpu fake): no adapter matching the cvar
// d3d12_adapter (checked before D3D12Provider::Create, which on
// failure would call xe::FatalError) or presenter not created.
std::unique_ptr<Backend> CreateD3D12Backend(GuestMemory&, const D3D12Options&);

// gow2::xenia::SetFatalErrorHook without including the Xenia headers:
// called by xe::FatalError (Xenia code, device lost) before
// exiting with code 5.
void SetFatalErrorHook(void (*hook)());

// --shader-capture: before _Exit (any thread), a running recording is stopped and the write queue drained, so the
// shots already queued reach the disk; shots still in the presenter's readback ring are lost. No-op otherwise.
void StopShaderCaptureBeforeExit();

// gow2::xenia::SetCvar (--gpu-cvar name=value): false if the cvar does not exist
// (even if it is not linked) or the value is not valid for its type.
bool SetXeniaCvar(std::string_view name, std::string_view value);

}  // namespace gpu
