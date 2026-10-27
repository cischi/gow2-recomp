// Camera matrices for DLSS FG (runtime/gpu/dlss_fg_math.h). CPU only.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gpu/dlss_fg_math.h"

namespace {
using Catch::Approx;

gpu::UberPostConstants MovingCamera() {
  gpu::UberPostConstants k{};
  const float c2[4] = {0, 0, 1e-1f, -1e-4f};
  const float c13[4] = {0.98f, 0.05f, 0.01f, 0.02f};
  const float c14[4] = {-0.04f, 1.02f, 0.03f, -0.01f};
  const float c15[4] = {0.10f, -0.20f, 0.30f, 0.99f};
  const float c16[4] = {0.5f, -0.3f, 0.2f, 0.05f};
  for (int i = 0; i < 4; ++i) {
    k.c2[i] = c2[i], k.c13[i] = c13[i], k.c14[i] = c14[i], k.c15[i] = c15[i], k.c16[i] = c16[i];
  }
  return k;
}
}  // namespace

TEST_CASE("dlss fg: ClipToPrevClip reproduces the shader", "[dlss_fg]") {
  const auto k = MovingCamera();
  const auto m = gpu::ClipToPrevClip(k);
  for (int i = 0; i < 20; ++i) {
    const float sx = -0.95f + 0.1f * i, sy = 0.9f - 0.09f * i, z = 0.01f + 0.02f * i;
    // Transcription of upscale_motion_vectors.hlsl (static branch).
    const float d = 1.0f / (z * k.c2[2] - k.c2[3]);
    const int idx[3] = {0, 1, 3};
    float prev[3];
    for (int j = 0; j < 3; ++j)
      prev[j] = d * (sx * k.c13[idx[j]] + sy * k.c14[idx[j]] + k.c15[idx[j]]) + k.c16[idx[j]];
    const float v[4] = {sx * d, sy * d, z * d, d};
    float p[4] = {};
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) p[c] += v[r] * m[r * 4 + c];
    CHECK(p[0] / p[3] == Approx(prev[0] / prev[2]).margin(1e-4).epsilon(1e-4));
    CHECK(p[1] / p[3] == Approx(prev[1] / prev[2]).margin(1e-4).epsilon(1e-4));
    CHECK(p[3] == Approx(prev[2]).margin(1e-4).epsilon(1e-4));
    const float z_prev = (1.0f / prev[2] + k.c2[3]) / k.c2[2];  // the game's non-linear z: d = 1/(z*c2.z - c2.w)
    CHECK(p[2] / p[3] == Approx(z_prev).margin(1e-4).epsilon(1e-4));
  }
}

TEST_CASE("dlss fg: Invert4x4", "[dlss_fg]") {
  const auto m = gpu::ClipToPrevClip(MovingCamera());
  const auto inv = gpu::Invert4x4(m);
  REQUIRE(inv.has_value());
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int i = 0; i < 4; ++i) s += m[r * 4 + i] * (*inv)[i * 4 + c];
      CHECK(s == Approx(r == c ? 1.0f : 0.0f).margin(1e-5));
    }
  gpu::Mat4 sing{};
  for (int i = 0; i < 16; ++i) sing[i] = static_cast<float>(i % 4);  // equal rows
  CHECK_FALSE(gpu::Invert4x4(sing).has_value());
}

TEST_CASE("dlss fg: ViewToClip", "[dlss_fg]") {
  const auto m = gpu::ViewToClip(1.0f, 16.0f / 9.0f, 10.0f, 1e6f);
  auto zndc = [&](float z) { return (z * m[2 * 4 + 2] + m[3 * 4 + 2]) / (z * m[2 * 4 + 3] + m[3 * 4 + 3]); };
  CHECK(zndc(10.0f) == Approx(1.0f));
  CHECK(zndc(1e7f) == Approx(0.0f).margin(1e-5));
}
