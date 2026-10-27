// Test textures on the direct queue of a D3D12Provider: creation, upload, readback and
// command lists executed to completion. For the tests of the runtime shaders (upscaler).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace gow2_test {

using Microsoft::WRL::ComPtr;

inline ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT f,
                                          D3D12_RESOURCE_STATES state,
                                          D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = f;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> r;
  REQUIRE(SUCCEEDED(d->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                               &desc, state, nullptr, IID_PPV_ARGS(&r))));
  return r;
}

inline ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* d, uint64_t size, D3D12_HEAP_TYPE heap) {
  D3D12_HEAP_PROPERTIES props{heap};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  const auto state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
  REQUIRE(SUCCEEDED(d->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))));
  return r;
}

inline void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  l->ResourceBarrier(1, &b);
}

// Records with `record` and executes on the direct queue, then waits for the GPU.
inline void RunOnGpu(const xe::ui::d3d12::D3D12Provider& p,
                     const std::function<void(ID3D12GraphicsCommandList*)>& record) {
  ID3D12Device* d = p.GetDevice();
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  REQUIRE(SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
  REQUIRE(SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
  REQUIRE(SUCCEEDED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
  record(list.Get());
  REQUIRE(SUCCEEDED(list->Close()));
  ID3D12CommandList* lists[] = {list.Get()};
  p.GetDirectQueue()->ExecuteCommandLists(1, lists);
  REQUIRE(SUCCEEDED(p.GetDirectQueue()->Signal(fence.Get(), 1)));
  REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
  REQUIRE(d->GetDeviceRemovedReason() == S_OK);
}

// Compact rows of `texel_bytes` bytes per texel; the texture goes from `state` to COPY_DEST and back.
inline void Upload(const xe::ui::d3d12::D3D12Provider& p, ID3D12Resource* tex, uint32_t texel_bytes,
                   const void* data, D3D12_RESOURCE_STATES state) {
  ID3D12Device* d = p.GetDevice();
  const D3D12_RESOURCE_DESC desc = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  d->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
  auto upload = MakeBuffer(d, total, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  REQUIRE(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
  const size_t row = size_t(desc.Width) * texel_bytes;
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + y * row, row);
  upload->Unmap(0, nullptr);
  RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) {
    Barrier(l, tex, state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, tex, D3D12_RESOURCE_STATE_COPY_DEST, state);
  });
}

// Compact rows; the texture goes from `state` to COPY_SOURCE and back.
inline std::vector<uint8_t> Readback(const xe::ui::d3d12::D3D12Provider& p, ID3D12Resource* tex, uint32_t texel_bytes,
                                     D3D12_RESOURCE_STATES state) {
  ID3D12Device* d = p.GetDevice();
  const D3D12_RESOURCE_DESC desc = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  d->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
  auto readback = MakeBuffer(d, total, D3D12_HEAP_TYPE_READBACK);
  RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) {
    Barrier(l, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  });
  const size_t row = size_t(desc.Width) * texel_bytes;
  std::vector<uint8_t> out(row * desc.Height);
  uint8_t* mapped = nullptr;
  REQUIRE(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(out.data() + y * row, mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, row);
  readback->Unmap(0, nullptr);
  return out;
}

}  // namespace gow2_test
