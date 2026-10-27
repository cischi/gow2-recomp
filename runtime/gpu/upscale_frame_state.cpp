// See upscale_frame_state.h.
#include "gpu/upscale_frame_state.h"

#include <cmath>

namespace gpu {

namespace {

bool IsIdentity(const UberPostConstants& k) {
  auto same = [](float a, float b, float eps) { return std::fabs(a - b) < eps; };
  return same(k.c13[0], 1, 1e-6f) && same(k.c13[1], 0, 1e-6f) && same(k.c14[0], 0, 1e-6f) &&
         same(k.c14[1], 1, 1e-6f) && same(k.c15[0], 0, 1e-6f) && same(k.c15[1], 0, 1e-6f) &&
         same(k.c16[0], 0, 1e-3f) && same(k.c16[1], 0, 1e-3f);
}

// NDC shift of the screen center at 1000 units of depth (uber post reprojection).
float CenterShift(const UberPostConstants& k) {
  constexpr float d = 1000.0f;
  const float w = d * k.c15[3] + k.c16[3];
  if (std::fabs(w) < 1e-6f) return 1.0f;
  return std::hypot((d * k.c15[0] + k.c16[0]) / w, (d * k.c15[1] + k.c16[1]) / w);
}

}  // namespace

bool UpscaleFrameState::NeedsReset(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, const UberPostConstants& k) {
  const uint32_t dims[4] = {rw, rh, ow, oh};
  const bool identity = IsIdentity(k);
  bool reset = broken_ || (identity && !previous_identity_) || CenterShift(k) > 0.25f;
  for (int i = 0; i < 4; ++i) reset |= dims[i] != dims_[i];
  for (int i = 0; i < 4; ++i) dims_[i] = dims[i];
  broken_ = false;
  previous_identity_ = identity;
  return reset;
}

std::pair<uint32_t, uint32_t> UpscaleOutputSize(uint32_t rw, uint32_t rh, uint32_t rect_w, uint32_t rect_h) {
  if (rect_w < rw || rect_h < rh) return {rw, rh};
  return {rect_w, rect_h};
}

}  // namespace gpu
