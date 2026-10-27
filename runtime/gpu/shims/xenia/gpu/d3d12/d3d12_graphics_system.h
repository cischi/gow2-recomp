// Shim of Xenia's src/xenia/gpu/d3d12/d3d12_graphics_system.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause).
//
// In Xenia D3D12GraphicsSystem creates the D3D12Provider and the presenter in
// Setup and the D3D12CommandProcessor in CreateCommandProcessor. In gow2 the
// GraphicsSystem is the shim of xenia/gpu/graphics_system.h (bridge to the
// guest registers): this subclass receives the already created provider (not
// owned, must outlive the GraphicsSystem) and the presenter, which may be
// nullptr (IssueSwap then does not present). The D3D12CommandProcessor is
// created by the factory of gpu::CreateXeniaBackend:
//   [](GraphicsSystem* gs) { return std::make_unique<D3D12CommandProcessor>(
//       static_cast<D3D12GraphicsSystem*>(gs), gs->kernel_state()); }
#ifndef XENIA_GPU_D3D12_D3D12_GRAPHICS_SYSTEM_H_
#define XENIA_GPU_D3D12_D3D12_GRAPHICS_SYSTEM_H_

#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12GraphicsSystem : public GraphicsSystem {
 public:
  D3D12GraphicsSystem(Memory* memory, ui::d3d12::D3D12Provider* provider, ui::Presenter* presenter)
      : GraphicsSystem(memory, provider, presenter) {}

  ui::d3d12::D3D12Provider* provider() const {
    return static_cast<ui::d3d12::D3D12Provider*>(GraphicsSystem::provider());
  }

  // For the `[gpu] device removed reason=...` line of OnHostGpuLossFromAnyThread.
  uint32_t gow2_DeviceRemovedReason() const override {
    ui::d3d12::D3D12Provider* d3d12_provider = provider();
    ID3D12Device* device = d3d12_provider ? d3d12_provider->GetDevice() : nullptr;
    return device ? uint32_t(device->GetDeviceRemovedReason()) : 0;
  }
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_GRAPHICS_SYSTEM_H_
