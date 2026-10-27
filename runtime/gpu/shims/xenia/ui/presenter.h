// Shim of Xenia's src/xenia/ui/presenter.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause).
//
// In Xenia the Presenter owns the window surface, the swap chain, the scaling
// effects and the UI drawing. In gow2 presentation lives in runtime/gpu: only
// the interface the command processor uses in IssueSwap remains here:
//  - RefreshGuestOutput(width, height, aspect x, aspect y, refresher):
//    the refresher receives a backend-specific context
//    (D3D12Presenter::D3D12GuestOutputRefreshContext) and writes the image;
//  - GuestOutputRefreshContext with SetIs8bpc.
// In Xenia RefreshGuestOutput is not virtual (it does the common work and calls
// RefreshGuestOutputImpl); here it is pure virtual and gow2's presenter
// implements it. The GraphicsSystem accepts a nullptr Presenter: IssueSwap then
// does nothing.
#ifndef XENIA_UI_PRESENTER_H_
#define XENIA_UI_PRESENTER_H_

#include <cstdint>
#include <functional>

namespace xe {
namespace ui {

class Presenter {
 public:
  class GuestOutputRefreshContext {
   public:
    GuestOutputRefreshContext(const GuestOutputRefreshContext& context) = delete;
    GuestOutputRefreshContext& operator=(const GuestOutputRefreshContext& context) = delete;
    virtual ~GuestOutputRefreshContext() = default;

    // Like Xenia: the source has at most 8 bits of precision (false if
    // never called).
    void SetIs8bpc(bool is_8bpc) { is_8bpc_out_ref_ = is_8bpc; }

   protected:
    explicit GuestOutputRefreshContext(bool& is_8bpc_out_ref) : is_8bpc_out_ref_(is_8bpc_out_ref) {
      is_8bpc_out_ref = false;
    }

   private:
    bool& is_8bpc_out_ref_;
  };

  virtual ~Presenter() = default;

  // Like Xenia: returns whether the refresher was called and returned
  // true. The refresher must submit all its work to the host GPU before
  // returning.
  virtual bool RefreshGuestOutput(uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                                  uint32_t display_aspect_ratio_x, uint32_t display_aspect_ratio_y,
                                  std::function<bool(GuestOutputRefreshContext& context)> refresher) = 0;

 protected:
  Presenter() = default;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_PRESENTER_H_
