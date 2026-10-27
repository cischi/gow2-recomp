// Shim of Xenia's src/xenia/ui/d3d12/d3d12_presenter.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause).
//
// Xenia's D3D12Presenter (swap chain, scaling effects, UI drawing) is not
// vendored: presentation lives in runtime/gpu. What
// D3D12CommandProcessor::IssueSwap uses remains here:
//  - kGuestOutputFormat (R10G10B10A2_UNORM) and kGuestOutputInternalState
//    (PIXEL_SHADER_RESOURCE), same values as Xenia;
//  - D3D12GuestOutputRefreshContext with resource_uav_capable(): the resource
//    (kGuestOutputFormat format, usable as UAV) the command processor writes
//    the front buffer to; on entry to the refresher it is in
//    kGuestOutputInternalState and the refresher must return it there before finishing.
// D3D12Presenter stays abstract: gow2's presenter implements
// RefreshGuestOutput (Presenter, xenia/ui/presenter.h).
#ifndef XENIA_UI_D3D12_D3D12_PRESENTER_H_
#define XENIA_UI_D3D12_D3D12_PRESENTER_H_

#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/presenter.h"

namespace xe {
namespace ui {
namespace d3d12 {

class D3D12Presenter : public Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  // Like Xenia: the refresher must use the provider's direct queue.
  class D3D12GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}

    ID3D12Resource* resource_uav_capable() const { return resource_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  };

 protected:
  D3D12Presenter() = default;
};

}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_D3D12_PRESENTER_H_
