// Per-frame rules of the upscaler: when FSR
// must forget the previous frames and at what size to reconstruct.
#pragma once

#include <cstdint>
#include <utility>

#include "gpu/upscale_passes.h"

namespace gpu {

class UpscaleFrameState {
 public:
  // For a frame with capture (render rw x rh, output ow x oh, uber post constants): true on the first frame,
  // after a frame without capture, when the dimensions change, on the switch to an identity reprojection (UE3
  // zeroes the motion at a camera cut) and when the screen center moves by more than 1/8 of the
  // screen in one frame.
  bool NeedsReset(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, const UberPostConstants& k);
  // Frame without uber post (loading, menu): the next one with capture restarts from zero.
  void NoteFrameWithoutCapture() { broken_ = true; }

 private:
  bool broken_ = true;
  bool previous_identity_ = false;
  uint32_t dims_[4] = {};
};

// FSR output size for a draw rectangle: the rectangle, or the render resolution if the
// rectangle is smaller on one axis (native anti-aliasing; the presenter then downscales).
std::pair<uint32_t, uint32_t> UpscaleOutputSize(uint32_t rw, uint32_t rh, uint32_t rect_w, uint32_t rect_h);

}  // namespace gpu
