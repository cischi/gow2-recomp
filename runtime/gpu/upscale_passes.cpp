// See upscale_passes.h.
#include "gpu/upscale_passes.h"

#include <cmath>
#include <cstring>

#include "log.h"
#include "shaders/upscale_motion_vectors.h"
#include "shaders/upscale_reactive.h"
#include "shaders/fg_hudless.h"

namespace gpu {

namespace {

constexpr uint32_t kRampWords = 256 + 128 * 3;

}  // namespace

DXGI_FORMAT ShaderViewFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
  }
}

float UpscalerMipBias(uint32_t render_w, uint32_t output_w) {
  return render_w && output_w > render_w ? std::log2(float(render_w) / float(output_w)) : 0.0f;
}

uint32_t CopySizeOfGuestTexture(uint32_t guest, uint32_t scale, uint64_t resource) {
  return resource >= uint64_t(guest) * scale ? guest * scale : guest;
}

bool CameraAtRest(const UberPostConstants& k) {
  auto within = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
  const bool rotation = within(k.c13[0], 1, 1e-6f) && within(k.c13[1], 0, 1e-6f) && within(k.c13[3], 0, 1e-6f) &&
                        within(k.c14[0], 0, 1e-6f) && within(k.c14[1], 1, 1e-6f) && within(k.c14[3], 0, 1e-6f);
  return rotation && within(k.c15[0], 0, 1e-3f) && within(k.c15[1], 0, 1e-3f) && within(k.c15[3], 1, 1e-3f) &&
         within(k.c16[0], 0, 0.02f) && within(k.c16[1], 0, 0.02f) && within(k.c16[3], 0, 0.01f);
}

// Compute shader with the root signature in the bytecode (RootSignature attribute).
static bool CreateCompute(ID3D12Device* device, const void* code, size_t size,
                          Microsoft::WRL::ComPtr<ID3D12RootSignature>& root,
                          Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.CS = {code, size};
  if (FAILED(device->CreateRootSignature(0, code, size, IID_PPV_ARGS(&root)))) return false;
  desc.pRootSignature = root.Get();
  return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
}

std::unique_ptr<UpscalePasses> UpscalePasses::Create(ID3D12Device* device) {
  std::unique_ptr<UpscalePasses> p(new UpscalePasses(device));
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = kSlots * kDescriptorsPerSlot;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = uint64_t(kSlots) * kRampWords * 4;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&p->heap_))) ||
      !CreateCompute(device, kUpscaleMotionVectorsCs, sizeof(kUpscaleMotionVectorsCs), p->mv_root_, p->mv_pso_) ||
      !CreateCompute(device, kUpscaleReactiveCs, sizeof(kUpscaleReactiveCs), p->reactive_root_, p->reactive_pso_) ||
      !CreateCompute(device, kFgHudlessCs, sizeof(kFgHudlessCs), p->hudless_root_, p->hudless_pso_) ||
      FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&p->ramp_upload_))) ||
      FAILED(p->ramp_upload_->Map(0, nullptr, reinterpret_cast<void**>(&p->ramp_mapped_)))) {
    LogLine("[gpu] upscaler: motion vector or reactive mask pipelines not created");
    return nullptr;
  }
  p->descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return p;
}

D3D12_CPU_DESCRIPTOR_HANDLE UpscalePasses::Cpu(uint32_t slot, uint32_t i) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(slot % kSlots * kDescriptorsPerSlot + i) * descriptor_size_;
  return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE UpscalePasses::Gpu(uint32_t slot, uint32_t i) const {
  D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
  h.ptr += uint64_t(slot % kSlots * kDescriptorsPerSlot + i) * descriptor_size_;
  return h;
}

void UpscalePasses::MotionVectors(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, ID3D12Resource* velocity,
                                  ID3D12Resource* mv, const UberPostConstants& k, uint32_t slot) {
  const D3D12_RESOURCE_DESC out = mv->GetDesc(), vel = velocity->GetDesc();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateShaderResourceView(depth, &srv, Cpu(slot, 0));
  srv.Format = DXGI_FORMAT_R16G16_UNORM;  // tf3 k_16_16 (risorsa R16G16_TYPELESS in Xenia)
  device_->CreateShaderResourceView(velocity, &srv, Cpu(slot, 1));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R16G16_FLOAT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(mv, nullptr, &uav, Cpu(slot, 2));

  uint32_t constants[29];
  std::memcpy(constants, &k, sizeof(k));
  constants[24] = uint32_t(out.Width);
  constants[25] = out.Height;
  constants[26] = uint32_t(vel.Width);
  constants[27] = vel.Height;
  constants[28] = CameraAtRest(k) ? 1u : 0u;
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(mv_root_.Get());
  cmd->SetComputeRoot32BitConstants(0, 29, constants, 0);
  cmd->SetComputeRootDescriptorTable(1, Gpu(slot, 0));
  cmd->SetPipelineState(mv_pso_.Get());
  cmd->Dispatch((uint32_t(out.Width) + 7) / 8, (out.Height + 7) / 8, 1);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = mv;
  cmd->ResourceBarrier(1, &b);
}


void UpscalePasses::ReactiveMask(ID3D12GraphicsCommandList* cmd, ID3D12Resource* prehud, ID3D12Resource* guest_output,
                                 const GammaRamp& ramp, ID3D12Resource* reactive, uint32_t slot) {
  uint32_t* words = ramp_mapped_ + size_t(slot % kSlots) * kRampWords;
  std::memcpy(words, ramp.table, sizeof(ramp.table));
  std::memcpy(words + 256, ramp.pwl_rgb, sizeof(ramp.pwl_rgb));

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  ID3D12Resource* textures[] = {prehud, guest_output};
  for (uint32_t i = 0; i < 2; ++i) {
    srv.Format = ShaderViewFormat(textures[i]->GetDesc().Format);
    device_->CreateShaderResourceView(textures[i], &srv, Cpu(slot, 3 + i));
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC buf{};
  buf.Format = DXGI_FORMAT_R32_UINT;
  buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  buf.Buffer.FirstElement = uint64_t(slot % kSlots) * kRampWords;
  buf.Buffer.NumElements = kRampWords;
  device_->CreateShaderResourceView(ramp_upload_.Get(), &buf, Cpu(slot, 5));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R8_UNORM;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(reactive, nullptr, &uav, Cpu(slot, 6));

  const D3D12_RESOURCE_DESC out = reactive->GetDesc();
  const uint32_t constants[4] = {ramp.pwl ? 1u : 0u, 0, 0, 0};
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(reactive_root_.Get());
  cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
  cmd->SetComputeRootDescriptorTable(1, Gpu(slot, 3));
  cmd->SetPipelineState(reactive_pso_.Get());
  cmd->Dispatch((uint32_t(out.Width) + 7) / 8, (out.Height + 7) / 8, 1);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = reactive;
  cmd->ResourceBarrier(1, &b);
}

void UpscalePasses::Hudless(ID3D12GraphicsCommandList* cmd, ID3D12Resource* base, ID3D12Resource* prehud,
                            ID3D12Resource* mask, bool pwl, const D3D12_RECT& rect, ID3D12Resource* out,
                            uint32_t slot) {
  HudlessPass(cmd, base, prehud, mask, pwl, false, rect, out, slot, 8);
}

void UpscalePasses::SceneWithoutHud(ID3D12GraphicsCommandList* cmd, ID3D12Resource* guest_output,
                                    ID3D12Resource* prehud, ID3D12Resource* mask, bool pwl, ID3D12Resource* out,
                                    uint32_t slot) {
  const D3D12_RESOURCE_DESC o = out->GetDesc();
  HudlessPass(cmd, guest_output, prehud, mask, pwl, false, {0, 0, LONG(o.Width), LONG(o.Height)}, out, slot, 13);
}

void UpscalePasses::ComposeHud(ID3D12GraphicsCommandList* cmd, ID3D12Resource* upscaled, ID3D12Resource* guest_output,
                               ID3D12Resource* mask, ID3D12Resource* out, uint32_t slot) {
  const D3D12_RESOURCE_DESC o = out->GetDesc();
  HudlessPass(cmd, upscaled, guest_output, mask, false, true, {0, 0, LONG(o.Width), LONG(o.Height)}, out, slot, 18);
}

void UpscalePasses::HudlessPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* base, ID3D12Resource* prehud,
                                ID3D12Resource* mask, bool pwl, bool raw, const D3D12_RECT& rect, ID3D12Resource* out,
                                uint32_t slot, uint32_t first) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  ID3D12Resource* textures[] = {base, prehud, mask};
  for (uint32_t i = 0; i < 3; ++i) {
    srv.Format = ShaderViewFormat(textures[i]->GetDesc().Format);
    device_->CreateShaderResourceView(textures[i], &srv, Cpu(slot, first + i));
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC buf{};
  buf.Format = DXGI_FORMAT_R32_UINT;
  buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  buf.Buffer.FirstElement = uint64_t(slot % kSlots) * kRampWords;
  buf.Buffer.NumElements = kRampWords;
  device_->CreateShaderResourceView(ramp_upload_.Get(), &buf, Cpu(slot, first + 3));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(out, nullptr, &uav, Cpu(slot, first + 4));
  const D3D12_RESOURCE_DESC o = out->GetDesc();
  const uint32_t constants[8] = {pwl ? 1u : 0u,         raw ? 1u : 0u,        0, 0, uint32_t(rect.left),
                                 uint32_t(rect.top), uint32_t(rect.right), uint32_t(rect.bottom)};
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(hudless_root_.Get());
  cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
  cmd->SetComputeRootDescriptorTable(1, Gpu(slot, first));
  cmd->SetPipelineState(hudless_pso_.Get());
  cmd->Dispatch((uint32_t(o.Width) + 7) / 8, (o.Height + 7) / 8, 1);
}

}  // namespace gpu
