// Temporal upscaler: common interface for FSR (FidelityFX API,
// upscaler_fsr.cpp) and DLSS (separate bridge gow2_dlss.dll, upscaler_dlss.cpp).
//
// Conventions (the same for FSR and DLSS):
//  - color, depth and motion vectors at render resolution, in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
//    output at output resolution in D3D12_RESOURCE_STATE_UNORDERED_ACCESS; states unchanged on exit.
//  - inverted, infinite depth (UE3's: 1 at near, 0 at infinity), R32_FLOAT;
//  - R16G16_FLOAT motion vectors from the current pixel to its position in the previous frame, multiplied by
//    mv_scale to get them in render pixels; no jitter;
//  - jitter in render pixels, in the sequence given by JitterPhaseCount/JitterOffset.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "settings.h"

namespace gpu {

struct UpscaleFrame {
  ID3D12GraphicsCommandList* cmd;
  ID3D12Resource *color, *depth, *motion, *output;
  // R8_UNORM reactive mask at render resolution (1 = current frame only: HUD), or nullptr.
  ID3D12Resource* reactive;
  uint32_t render_w, render_h, output_w, output_h;
  float jitter_x, jitter_y;
  float mv_scale_x, mv_scale_y;
  bool reset;
  float sharpness;  // 0..1, < 0 = sharpening off
  float frame_ms;
  bool debug_view;  // upscaler debug view instead of the image (if it has one)
};

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  virtual const std::string& Name() const = 0;  // es. "FSR 3.1.5"
  virtual uint32_t JitterPhaseCount(uint32_t render_w, uint32_t output_w) = 0;
  virtual void JitterOffset(uint32_t index, uint32_t phases, float& x, float& y) = 0;
  // Records the work into frame.cmd. false: error already in the log.
  virtual bool Dispatch(const UpscaleFrame& frame) = 0;
};

// nullptr + [gpu] line with the reason (DLL missing from dll_dir, creation rejected by the driver...). The DLLs are
// searched only in dll_dir (the gow2.exe folder in the game). max_*: maximum context dimensions.
std::unique_ptr<Upscaler> CreateFsrUpscaler(ID3D12Device* device, uint32_t max_render_w, uint32_t max_render_h,
                                            uint32_t max_output_w, uint32_t max_output_h,
                                            const std::filesystem::path& dll_dir);
// DLSS through gow2_dlss.dll in dll_dir (runtime/gpu/dlss_bridge/); nullptr + [gpu] line if missing or rejected.
std::unique_ptr<Upscaler> CreateDlssUpscaler(ID3D12Device* device, uint32_t max_render_w, uint32_t max_render_h,
                                             uint32_t max_output_w, uint32_t max_output_h,
                                             const std::filesystem::path& dll_dir,
                                             settings::DlssPreset preset = settings::DlssPreset::kDefault);

}  // namespace gpu
