// Per-frame rules of the upscaler (runtime/gpu/upscale_frame_state.h): history reset and
// output size. CPU only.
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "gpu/upscale_frame_state.h"

namespace {

gpu::UberPostConstants Still() {
  gpu::UberPostConstants k{};
  const float c13[4] = {1, 0, 0, 0}, c14[4] = {0, 1, 0, 0}, c15[4] = {0, 0, 0.999f, 1}, c16[4] = {0, 0.000031f, -9.99f, 0};
  std::memcpy(k.c13, c13, 16);
  std::memcpy(k.c14, c14, 16);
  std::memcpy(k.c15, c15, 16);
  std::memcpy(k.c16, c16, 16);
  return k;
}

// Camera in normal movement (spike frame 934).
gpu::UberPostConstants Moving() {
  gpu::UberPostConstants k = Still();
  k.c13[1] = -0.000682f;
  k.c14[0] = 0.000216f;
  k.c15[0] = -0.00033f;
  k.c15[1] = -0.000916f;
  k.c16[0] = -0.617188f;
  k.c16[1] = -0.337982f;
  k.c16[3] = 2.869385f;
  return k;
}

}  // namespace

TEST_CASE("upscaler: history reset", "[upscaler]") {
  gpu::UpscaleFrameState s;
  CHECK(s.NeedsReset(1280, 720, 2560, 1440, Moving()));       // first frame
  CHECK_FALSE(s.NeedsReset(1280, 720, 2560, 1440, Moving()));  // normal movement
  CHECK_FALSE(s.NeedsReset(1280, 720, 2560, 1440, Moving()));
  s.NoteFrameWithoutCapture();                                 // loading, menu
  CHECK(s.NeedsReset(1280, 720, 2560, 1440, Moving()));
  CHECK(s.NeedsReset(1280, 720, 1920, 1080, Moving()));         // window resized
  CHECK(s.NeedsReset(2560, 1440, 1920, 1080, Moving()));        // internal resolution changed
  CHECK(s.NeedsReset(2560, 1440, 1920, 1080, Still()));         // from movement to identical reprojection (cut)
  CHECK_FALSE(s.NeedsReset(2560, 1440, 1920, 1080, Still()));   // still camera
  CHECK_FALSE(s.NeedsReset(2560, 1440, 1920, 1080, Moving()));
  gpu::UberPostConstants jump = Moving();
  jump.c15[0] = 0.4f;  // the screen center moves a lot in one frame: cut
  CHECK(s.NeedsReset(2560, 1440, 1920, 1080, jump));
}

TEST_CASE("upscaler: output size", "[upscaler]") {
  CHECK(gpu::UpscaleOutputSize(1280, 720, 2560, 1440) == std::pair<uint32_t, uint32_t>{2560, 1440});
  CHECK(gpu::UpscaleOutputSize(2560, 1440, 2560, 1440) == std::pair<uint32_t, uint32_t>{2560, 1440});
  // Rectangle smaller than the render resolution: native anti-aliasing, then the presenter downscales.
  CHECK(gpu::UpscaleOutputSize(2560, 1440, 1280, 720) == std::pair<uint32_t, uint32_t>{2560, 1440});
  CHECK(gpu::UpscaleOutputSize(2560, 1440, 2000, 1500) == std::pair<uint32_t, uint32_t>{2560, 1440});
}
