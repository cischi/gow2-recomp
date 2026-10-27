// HDR10 output math (runtime/hdr_math).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "hdr_math.h"

using Catch::Approx;
using namespace hdr;

namespace {
Approx A(double v) { return Approx(v).epsilon(1e-4); }
void CheckAll(Rgb c, double v) {
  CHECK(c.r == A(v));
  CHECK(c.g == A(v));
  CHECK(c.b == A(v));
}
}  // namespace

TEST_CASE("hdr: EOTF sRGB", "[hdr]") {
  CHECK(SrgbToLinear(0) == 0);
  CHECK(SrgbToLinear(1) == 1);
  CHECK(SrgbToLinear(0.5f) == A(0.214041));
  CHECK(SrgbToLinear(0.02f) == A(0.0015480));
  CHECK(SrgbToLinear(-1) == 0);
  CHECK(SrgbToLinear(2) == 1);
}

TEST_CASE("hdr: PQ", "[hdr]") {
  CHECK(PqFromNits(10000) == 1);
  CHECK(PqFromNits(0) == Approx(7.31e-7).margin(1e-8));
  CHECK(PqFromNits(80) == A(0.485857));
  CHECK(PqFromNits(100) == A(0.508078));
  CHECK(PqFromNits(203) == A(0.580689));
  CHECK(PqFromNits(1000) == A(0.751827));
  CHECK(PqFromNits(20000) == 1);
  for (float n : {1.0f, 80.0f, 203.0f, 1000.0f, 4000.0f}) CHECK(NitsFromPq(PqFromNits(n)) == Approx(n).epsilon(1e-3));
}

TEST_CASE("hdr: BT.709 -> BT.2020", "[hdr]") {
  const Rgb w = Bt709ToBt2020({1, 1, 1});
  CHECK(w.r == Approx(1).margin(1e-6));
  CHECK(w.g == Approx(1).margin(1e-6));
  CHECK(w.b == Approx(1).margin(1e-6));
  const Rgb r = Bt709ToBt2020({1, 0, 0});
  CHECK(r.r == A(0.627404));
  CHECK(r.g == A(0.069097));
  CHECK(r.b == A(0.016391));
  const Rgb g = Bt709ToBt2020({0, 1, 0});
  CHECK(g.r == A(0.329283));
  CHECK(g.g == A(0.919540));
  CHECK(g.b == A(0.088013));
  const Rgb b = Bt709ToBt2020({0, 0, 1});
  CHECK(b.r == A(0.043313));
  CHECK(b.g == A(0.011362));
  CHECK(b.b == A(0.895595));
}

TEST_CASE("hdr: shoulder", "[hdr]") {
  CHECK(Shoulder(0.5f, 5) == 0.5f);
  CHECK(Shoulder(1, 5) == 1);
  CHECK(Shoulder(2, 5) == A(1.8));
  CHECK(Shoulder(3, 5) == A(2.333333));
  CHECK(Shoulder(1e6f, 5) == Approx(5).epsilon(1e-3));
  CHECK((Shoulder(1.001f, 5) - 1) / 0.001 == Approx(1).epsilon(1e-2));
  CHECK(Shoulder(3, 1) == 1);
  CHECK(Shoulder(3, 0.5f) == 1);
}

TEST_CASE("hdr: uber post tonemapping", "[hdr]") {
  UberTonemapConstants k{{0, 0, -0.003f, 0.6f}, {1.25f, 1.25f, 1.43f, 0}, {1.4f, 1.4f, 1.35f, 0},
                         {0.12f, 0.236f, 0.044f, 0}, {1, 1, 1, 0.4545f}, {0, 0, 0, 0}, 1e-4f};
  UberTonemapOut o = UberTonemap({1.2f, 0.6f, 0.3f}, k);
  CHECK(o.y.r == A(1));
  CHECK(o.y.g == A(0.737077));
  CHECK(o.y.b == A(0.529992));
  CHECK(o.y_unclamped.r == A(1.442152));
  CHECK(o.y_unclamped.g == A(0.784767));
  CHECK(o.y_unclamped.b == A(0.577682));

  k = {{0, 0, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {1, 1, 1, 0.4545f}, {0, 0, 0, 0}, 1e-4f};
  o = UberTonemap({2, 0.5f, 0}, k);
  CHECK(o.y.r == A(1));
  CHECK(o.y.g == A(0.5));
  CHECK(o.y.b == A(1e-4));
  CHECK(o.y_unclamped.r == A(2));
  CHECK(o.y_unclamped.g == A(0.5));
  CHECK(o.y_unclamped.b == A(1e-4));

  // c10 and c9 differ per channel; pre - c5 = (0.25, 1.52, -0.2): above 1, and negative.
  k = {{0.05f, -0.02f, 0.4f, 0.7f}, {1.1f, 0.9f, 1.3f, 0}, {1.2f, 1.5f, 0.8f, 0}, {0.1f, 0.2f, 0.3f, 0},
       {0.9f, 1.2f, 0.7f, 0.4545f}, {0.05f, -0.1f, 0.2f, 0}, 1e-4f};
  o = UberTonemap({0.3f, 1.5f, 0.2f}, k);
  CHECK(o.y.r == A(0.351801));
  CHECK(o.y.g == A(0.827838));
  CHECK(o.y.b == A(0.274845));
  CHECK(o.y_unclamped.r == A(0.486120));
  CHECK(o.y_unclamped.g == A(1.633754));
  CHECK(o.y_unclamped.b == A(0.379316));
}

TEST_CASE("hdr: HUD mask", "[hdr]") {
  CHECK(HudMask({0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}) == 0);
  CHECK(HudMask({64 / 1023.0f, 0, 0}, {0, 0, 0}) == A(1));
  CHECK(HudMask({0, 34 / 1023.0f, 0}, {0, 0, 0}) == A(0.5));
  CHECK(HudMask({0, 0, 1}, {0, 0, 0}) == 1);
  CHECK(HudMask({0, 0, 19 / 1023.0f}, {0, 0, 0}) == A(0.15625));
  CHECK(HudMask({0.5f, 0.5f, 0.5f}, {0.5f + 19 / 1023.0f, 0.5f, 0.5f}) == A(0.15625));
  CHECK(HudMask({0.5f, 0.5f, 0.5f}, {0.5f, 0.5f + 19 / 1023.0f, 0.5f}) == A(0.15625));
  CHECK(HudMask({0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f + 19 / 1023.0f}) == A(0.15625));
}

TEST_CASE("hdr: residual", "[hdr]") {
  const float c9w = 1 / 2.2f;
  CheckAll(Residual({0.5f, 0.5f, 0.5f}, {1, 1, 1}, {2, 2, 2}, c9w, 5, 0), 0.5);
  CheckAll(Residual({1, 1, 1}, {1, 1, 1}, {3, 3, 3}, c9w, 5, 0), 1.333333);
  CheckAll(Residual({1, 1, 1}, {1, 1, 1}, {3, 3, 3}, c9w, 5, 0.5f), 0.666667);
  CHECK(Residual({1, 1, 1}, {1, 1, 1}, {0.5f, 0.5f, 0.5f}, c9w, 5, 0) == Rgb{0, 0, 0});
  CHECK(Residual({1, 1, 1}, {1, 1, 1}, {3, 3, 3}, c9w, 1, 0) == Rgb{0, 0, 0});
  const Rgb r = Residual({0.3f, 0.6f, 0.9f}, {0.8f, 0.5f, 1}, {1.6f, 0.9f, 1}, 0.4545f, 4, 0.25f);
  CHECK(r.r == A(0.224969));
  CHECK(r.g == A(0.358396));
  CHECK(r.b == 0);
}

TEST_CASE("hdr: PQ composition", "[hdr]") {
  CheckAll(ComposePq({0.5f, 0.5f, 0.5f}, {0, 0, 0}, 203), 0.427019);
  CheckAll(ComposePq({1, 1, 1}, {1.333333f, 1.333333f, 1.333333f}, 200), 0.669148);
  const Rgb c = ComposePq({1, 0.5f, 0.2f}, {0.5f, 0.1f, 0.8f}, 250);
  CHECK(c.r == A(0.610749));
  CHECK(c.g == A(0.508550));
  CHECK(c.b == A(0.578918));
  const Rgb b = ComposePq({0, 0, 0}, {0, 0, 0}, 203);
  CHECK(b.r == Approx(7.31e-7).margin(1e-8));
  CHECK(b.g == Approx(7.31e-7).margin(1e-8));
  CHECK(b.b == Approx(7.31e-7).margin(1e-8));
}

TEST_CASE("hdr: levels", "[hdr]") {
  CHECK(ChooseLevels(0, 0, {true, 418, 240, ""}) == Levels{418, 240});
  CHECK(ChooseLevels(0, 0, {true, 0, 0, ""}) == Levels{1000, 203});
  CHECK(ChooseLevels(0, 0, {false, 418, 80, ""}) == Levels{1000, 203});
  CHECK(ChooseLevels(1500, 300, {true, 418, 240, ""}) == Levels{1500, 300});
  CHECK(ChooseLevels(400, 0, {true, 0, 600, ""}) == Levels{400, 400});
  CHECK(ChooseLevels(1500, 300, {false, 418, 80, ""}) == Levels{1500, 300});
  CHECK(ChooseLevels(0, 0, {true, 418, 0, ""}) == Levels{418, 203});
  CHECK(ChooseLevels(0, 0, {true, 0, 240, ""}) == Levels{1000, 240});
}

TEST_CASE("hdr: window output", "[hdr]") {
  const std::vector<Rect> outs = {{0, 0, 1920, 1080}, {1920, 0, 4480, 1440}};
  CHECK(ChooseOutput({1800, 100, 2600, 700}, outs) == 1);
  CHECK(ChooseOutput({100, 100, 900, 700}, outs) == 0);
  CHECK(ChooseOutput({5000, 0, 5200, 100}, outs) == -1);
  CHECK(ChooseOutput({0, 0, 100, 100}, {}) == -1);
  const std::vector<Rect> tie = {{0, 0, 1000, 1080}, {1000, 0, 2000, 1080}};
  CHECK(ChooseOutput({500, 0, 1500, 100}, tie) == 0);  // tie: the first
  const std::vector<Rect> neg = {{-1920, 0, 0, 1080}, {0, 0, 1920, 1080}};
  CHECK(ChooseOutput({-1000, 100, -200, 500}, neg) == 0);
  CHECK(ChooseOutput({-100, 100, 700, 500}, neg) == 1);
}
