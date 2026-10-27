// Runtime passes around the upscaler: motion vectors
// from UE3's uber post (shaders/upscale_motion_vectors.hlsl) and the HUD reactive mask with the game's
// gamma ramp (shaders/upscale_reactive.hlsl).
//
// Descriptors in a dedicated heap, one space per `slot`: the caller uses different slots for frames
// still in flight on the GPU. The passes set the heap, root signature and pipeline of the command list:
// whoever records afterwards must set its own again.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace gpu {

// Uber post pixel shader constants used by the motion vectors (registers c2, c13..c17).
struct UberPostConstants {
  float c2[4], c13[4], c14[4], c15[4], c16[4], c17[4];
};

// Still camera: exactly identical rotation and translation within the game's rounding (with the camera
// still the constants stay for thousands of frames at values like -2^-7, 2^-10, ~0.4 px of fake movement at
// every frame, invisible in the motion blur but blurring the upscaler's history). Then static pixels have
// zero motion vectors.
// shortcut: a very slow translation with identical rotation (within the thresholds) is treated as still, upgrade:
// thresholds derived from the matrix precision instead of fixed.
bool CameraAtRest(const UberPostConstants& k);

// Format of the SRV of a possibly TYPELESS resource (Xenia render targets and textures, capture copies); other
// formats unchanged.
DXGI_FORMAT ShaderViewFormat(DXGI_FORMAT format);

// LOD bias of the scene textures with an upscaler (AMD and NVIDIA recommendation): log2(render / output),
// 0 if the output is not wider than the render or if a dimension is 0.
float UpscalerMipBias(uint32_t render_w, uint32_t output_w);

// Side to copy of a guest texture `guest` wide at scale `scale`: guest * scale if the host resource
// (`resource`) is scaled, guest if it stayed native (draw_resolution_scale_threshold).
uint32_t CopySizeOfGuestTexture(uint32_t guest, uint32_t scale, uint64_t resource);

// The game's gamma ramp as Xenia's IssueSwap uses it: values of the DC_LUT_30_COLOR registers (table)
// and DC_LUT_PWL_DATA (PWL, 128 entries per channel, r g b alternating).
struct GammaRamp {
  bool pwl;
  uint32_t table[256];
  uint32_t pwl_rgb[128 * 3];
};

class UpscalePasses {
 public:
  static constexpr uint32_t kSlots = 2;
  // nullptr (with a [gpu] line) if the root signature, pipeline or heap cannot be created.
  static std::unique_ptr<UpscalePasses> Create(ID3D12Device* device);

  // depth R32_FLOAT and velocity R16G16 (UNORM) in NON_PIXEL_SHADER_RESOURCE; mv R16G16_FLOAT in
  // UNORDERED_ACCESS, same size as depth. States unchanged.
  void MotionVectors(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, ID3D12Resource* velocity,
                     ID3D12Resource* mv, const UberPostConstants& k, uint32_t slot);

  // R8_UNORM reactive mask (UAV, same size as prehud): from 0 where guest_output is the prehud ramp to 1
  // where it differs by at least 64/1023 (HUD). prehud and guest_output in NON_PIXEL_SHADER_RESOURCE, at
  // internal resolution. States unchanged.
  void ReactiveMask(ID3D12GraphicsCommandList* cmd, ID3D12Resource* prehud, ID3D12Resource* guest_output,
                    const GammaRamp& ramp, ID3D12Resource* reactive, uint32_t slot);

  // Frame generation (shaders/fg_hudless.hlsl): out (R10G10B10A2 UAV, output resolution) = base where the
  // mask is zero, prehud ramp where there is HUD. Uses the ramp written by ReactiveMask in the same slot.
  // rect (pixels of out): rectangle of the image in the back buffer (presenter fit/stretch); base, prehud and
  // mask cover the rectangle, outside is opaque black like the presenter's bands.
  // base, prehud, mask read (NON_PIXEL | PIXEL). States unchanged.
  void Hudless(ID3D12GraphicsCommandList* cmd, ID3D12Resource* base, ID3D12Resource* prehud, ID3D12Resource* mask,
               bool pwl, const D3D12_RECT& rect, ID3D12Resource* out, uint32_t slot);

  // Upscaler input without HUD: out (R10G10B10A2 UAV, render resolution) = guest output where the mask is
  // zero, prehud ramp where there is HUD. With the HUD inside, the upscaler dragged it along with the scene's motion vectors
  // (unreadable subtitles with the camera moving). ReactiveMask ramp in the same slot; states as Hudless.
  void SceneWithoutHud(ID3D12GraphicsCommandList* cmd, ID3D12Resource* guest_output, ID3D12Resource* prehud,
                       ID3D12Resource* mask, bool pwl, ID3D12Resource* out, uint32_t slot);

  // HUD over the upscaler output: out (R10G10B10A2 UAV, output resolution) = upscaled where the mask is
  // zero, guest output (bilinear) where there is HUD. States as Hudless.
  void ComposeHud(ID3D12GraphicsCommandList* cmd, ID3D12Resource* upscaled, ID3D12Resource* guest_output,
                  ID3D12Resource* mask, ID3D12Resource* out, uint32_t slot);

 private:
  static constexpr uint32_t kDescriptorsPerSlot = 24;
  // fg_hudless.hlsl with the descriptors from `first` (5: three textures, ramp, output).
  void HudlessPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* base, ID3D12Resource* over, ID3D12Resource* mask,
                   bool pwl, bool raw, const D3D12_RECT& rect, ID3D12Resource* out, uint32_t slot, uint32_t first);
  explicit UpscalePasses(ID3D12Device* device) : device_(device) {}
  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t slot, uint32_t i) const;
  D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t slot, uint32_t i) const;

  ID3D12Device* device_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t descriptor_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> mv_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> mv_pso_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> reactive_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> reactive_pso_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> hudless_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> hudless_pso_;
  // Ramp per slot (256 + 384 uint32), always mapped.
  Microsoft::WRL::ComPtr<ID3D12Resource> ramp_upload_;
  uint32_t* ramp_mapped_ = nullptr;
};

}  // namespace gpu
