// Shim of Xenia's src/xenia/ui/graphics_provider.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): GpuVendorID and the base class, without CreatePresenter and
// CreateImmediateDrawer (Xenia's presenter and immediate drawer are not
// used: presentation lives in runtime/gpu).
#ifndef XENIA_UI_GRAPHICS_PROVIDER_H_
#define XENIA_UI_GRAPHICS_PROVIDER_H_

#include <memory>

namespace xe {
namespace ui {

class GraphicsProvider {
 public:
  enum class GpuVendorID {
    kAMD = 0x1002,
    kApple = 0x106B,
    kArm = 0x13B5,
    kImagination = 0x1010,
    kIntel = 0x8086,
    kMicrosoft = 0x1414,
    kNvidia = 0x10DE,
    kQualcomm = 0x5143,
  };

  GraphicsProvider(const GraphicsProvider&) = delete;
  GraphicsProvider& operator=(const GraphicsProvider&) = delete;
  GraphicsProvider(GraphicsProvider&&) = delete;
  GraphicsProvider& operator=(GraphicsProvider&&) = delete;

  virtual ~GraphicsProvider() = default;

 protected:
  GraphicsProvider() = default;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GRAPHICS_PROVIDER_H_
