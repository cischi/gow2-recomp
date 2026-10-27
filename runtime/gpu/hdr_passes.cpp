// See hdr_passes.h.
#include "gpu/hdr_passes.h"

#include <cstdio>
#include <cstring>

#include "gpu/upscale_passes.h"
#include "log.h"
#include "shaders/hdr_compose_ps.h"
#include "shaders/hdr_compose_vs.h"
#include "shaders/hdr_reconstruct.h"

namespace gpu {

namespace {

constexpr uint32_t kRampWords = 256 + 128 * 3;
// Per slot: constants (512 bytes, address aligned to 256 for the CBV) then ramp.
constexpr uint32_t kCbBytes = 512, kSlotBytes = kCbBytes + kRampWords * 4;
static_assert(kSlotBytes % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

// cbuffer K of hdr_reconstruct.hlsl.
struct ReconstructConstants {
  float c[22][4];  // c0..c18, c253, c254, c255
  uint32_t filt[4];
  float scl[4];
};
static_assert(sizeof(ReconstructConstants) <= kCbBytes);

D3D12_SHADER_RESOURCE_VIEW_DESC TextureView(DXGI_FORMAT f) {
  D3D12_SHADER_RESOURCE_VIEW_DESC v{};
  v.Format = ShaderViewFormat(f);
  v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  v.Texture2D.MipLevels = 1;
  return v;
}

}  // namespace

std::unique_ptr<HdrPasses> HdrPasses::Create(ID3D12Device* device) {
  std::unique_ptr<HdrPasses> p(new HdrPasses(device));
  HRESULT hr = device->CreateRootSignature(0, kHdrReconstructCs, sizeof(kHdrReconstructCs),
                                           IID_PPV_ARGS(&p->reconstruct_root_));
  if (SUCCEEDED(hr)) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = p->reconstruct_root_.Get();
    cd.CS = {kHdrReconstructCs, sizeof(kHdrReconstructCs)};
    hr = device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&p->reconstruct_pso_));
  }
  if (SUCCEEDED(hr))
    hr = device->CreateRootSignature(0, kHdrComposePs, sizeof(kHdrComposePs), IID_PPV_ARGS(&p->compose_root_));
  if (SUCCEEDED(hr)) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
    gd.pRootSignature = p->compose_root_.Get();
    gd.VS = {kHdrComposeVs, sizeof(kHdrComposeVs)};
    gd.PS = {kHdrComposePs, sizeof(kHdrComposePs)};
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    gd.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&p->compose_pso_));
  }
  if (SUCCEEDED(hr)) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kDescriptorsPerSlot,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&p->heap_));
  }
  if (SUCCEEDED(hr)) {
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = uint64_t(kSlots) * kSlotBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&p->upload_));
  }
  if (SUCCEEDED(hr)) hr = p->upload_->Map(0, nullptr, reinterpret_cast<void**>(&p->mapped_));
  if (FAILED(hr)) {
    char line[64];
    std::snprintf(line, sizeof line, "[hdr] passes not created (0x%08X): SDR", unsigned(hr));
    LogLine(line);
    return nullptr;
  }
  p->descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return p;
}

D3D12_CPU_DESCRIPTOR_HANDLE HdrPasses::Cpu(uint32_t slot, uint32_t i) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(slot % kSlots * kDescriptorsPerSlot + i) * descriptor_size_;
  return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE HdrPasses::Gpu(uint32_t slot, uint32_t i) const {
  D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
  h.ptr += uint64_t(slot % kSlots * kDescriptorsPerSlot + i) * descriptor_size_;
  return h;
}

void HdrPasses::Reconstruct(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, ID3D12Resource* scene,
                            ID3D12Resource* bloom, ID3D12Resource* velocity, ID3D12Resource* prehud,
                            ID3D12Resource* guest_output, const HdrUberConstants& k, const GammaRamp& ramp,
                            float peak_over_paper, ID3D12Resource* residual, uint32_t slot) {
  const uint32_t s = slot % kSlots;
  uint8_t* base = mapped_ + size_t(s) * kSlotBytes;
  ReconstructConstants rc{};
  std::memcpy(rc.c, k.c, sizeof(k.c));
  std::memcpy(rc.c[19], k.c253, 16);
  std::memcpy(rc.c[20], k.c254, 16);
  std::memcpy(rc.c[21], k.c255, 16);
  for (int i = 0; i < 3; ++i) {
    rc.filt[i] = k.linear[i];
    rc.scl[i] = k.exp_scale[i];
  }
  rc.filt[3] = ramp.pwl ? 1u : 0u;
  rc.scl[3] = peak_over_paper;
  std::memcpy(base, &rc, sizeof(rc));
  std::memcpy(base + kCbBytes, ramp.table, sizeof(ramp.table));
  std::memcpy(base + kCbBytes + sizeof(ramp.table), ramp.pwl_rgb, sizeof(ramp.pwl_rgb));

  ID3D12Resource* textures[] = {depth, scene, bloom, velocity, prehud, guest_output};
  for (uint32_t i = 0; i < 6; ++i) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC v = TextureView(textures[i]->GetDesc().Format);
    device_->CreateShaderResourceView(textures[i], &v, Cpu(s, i));
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC buf{};
  buf.Format = DXGI_FORMAT_R32_UINT;
  buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  buf.Buffer.FirstElement = (uint64_t(s) * kSlotBytes + kCbBytes) / 4;
  buf.Buffer.NumElements = kRampWords;
  device_->CreateShaderResourceView(upload_.Get(), &buf, Cpu(s, 6));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(residual, nullptr, &uav, Cpu(s, 7));

  const D3D12_RESOURCE_DESC out = prehud->GetDesc();
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(reconstruct_root_.Get());
  cmd->SetComputeRootConstantBufferView(0, upload_->GetGPUVirtualAddress() + uint64_t(s) * kSlotBytes);
  cmd->SetComputeRootDescriptorTable(1, Gpu(s, 0));
  cmd->SetPipelineState(reconstruct_pso_.Get());
  cmd->Dispatch((uint32_t(out.Width) + 7) / 8, (out.Height + 7) / 8, 1);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = residual;
  cmd->ResourceBarrier(1, &b);
}

void HdrPasses::Compose(ID3D12GraphicsCommandList* cmd, ID3D12Resource* sdr, ID3D12Resource* pre_overlay,
                        ID3D12Resource* residual, const D3D12_RECT& rect, float paper_white_nits,
                        D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h, uint32_t slot, uint32_t use) {
  const uint32_t s = slot % kSlots, first = 8 + 3 * (use % 2);
  ID3D12Resource* textures[] = {sdr, pre_overlay, residual};
  for (uint32_t i = 0; i < 3; ++i) {
    // Absent: null descriptor (not read by the shader).
    const D3D12_SHADER_RESOURCE_VIEW_DESC v =
        TextureView(textures[i] ? textures[i]->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT);
    device_->CreateShaderResourceView(textures[i], &v, Cpu(s, first + i));
  }
  uint32_t k[8] = {uint32_t(rect.left), uint32_t(rect.top), uint32_t(rect.right), uint32_t(rect.bottom), 0,
                   residual ? 1u : 0u, pre_overlay ? 1u : 0u, 0};
  std::memcpy(&k[4], &paper_white_nits, 4);
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
  const D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
  cmd->RSSetViewports(1, &vp);
  cmd->RSSetScissorRects(1, &sc);
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRootSignature(compose_root_.Get());
  cmd->SetPipelineState(compose_pso_.Get());
  cmd->SetGraphicsRoot32BitConstants(0, 8, k, 0);
  cmd->SetGraphicsRootDescriptorTable(1, Gpu(s, first));
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(3, 1, 0, 0);
}

}  // namespace gpu
