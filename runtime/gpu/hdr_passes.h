// HDR10 output passes: reconstruction of the residual from
// UE3's uber post chain (shaders/hdr_reconstruct.hlsl) and PQ BT.2020 composition of the SDR image plus the
// residual (shaders/hdr_compose.hlsl). Numeric reference: runtime/hdr_math.
//
// Descriptors in a dedicated heap, one space per `slot` like UpscalePasses: the passes set the heap, root signature
// and pipeline of the command list, whoever records afterwards must set its own again.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

#include "gpu/upscale_passes.h"

namespace gpu {

// Uber post constants for the reconstruction (registers c0..c18, c253..c255) and fetch of tf1, tf2, tf3.
struct HdrUberConstants {
  float c[19][4];
  float c253[4], c254[4], c255[4];
  uint32_t linear[3];   // 1 = linear filter (mag_filter) of tf1, tf2, tf3
  float exp_scale[3];   // 2^exp_adjust of tf1, tf2, tf3
};

class HdrPasses {
 public:
  static constexpr uint32_t kSlots = 2;
  static std::unique_ptr<HdrPasses> Create(ID3D12Device* device);  // nullptr with a [hdr] line
  // residual RGBA16F UAV (size of prehud): rgb = residual in paper-white units, a = HUD mask.
  // depth, scene, bloom, velocity, prehud, guest_output in NON_PIXEL | PIXEL_SHADER_RESOURCE. States unchanged.
  void Reconstruct(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, ID3D12Resource* scene,
                   ID3D12Resource* bloom, ID3D12Resource* velocity, ID3D12Resource* prehud,
                   ID3D12Resource* guest_output, const HdrUberConstants& k, const GammaRamp& ramp,
                   float peak_over_paper, ID3D12Resource* residual, uint32_t slot);
  // rtv (R10G10B10A2, w x h) <- PQ BT.2020 of sdr (sRGB, w x h) + residual (bilinear in the rect; nullptr = none).
  // pre_overlay (w x h, nullptr = overlay closed): zero residual where sdr differs from it. use: 0 back buffer,
  // 1 HUD-less (distinct descriptors in the same slot). Inputs in PIXEL_SHADER_RESOURCE, rtv in RENDER_TARGET.
  void Compose(ID3D12GraphicsCommandList* cmd, ID3D12Resource* sdr, ID3D12Resource* pre_overlay,
               ID3D12Resource* residual, const D3D12_RECT& rect, float paper_white_nits,
               D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h, uint32_t slot, uint32_t use);

 private:
  // 0..6 SRV and 7 UAV of the reconstruction, then 3 SRV for each composition use.
  static constexpr uint32_t kDescriptorsPerSlot = 8 + 2 * 3;
  explicit HdrPasses(ID3D12Device* device) : device_(device) {}
  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t slot, uint32_t i) const;
  D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t slot, uint32_t i) const;

  ID3D12Device* device_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t descriptor_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> reconstruct_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> reconstruct_pso_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> compose_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_pso_;
  // Per slot: reconstruction constants and ramp (256 + 384 uint32), always mapped.
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
  uint8_t* mapped_ = nullptr;
};

}  // namespace gpu
