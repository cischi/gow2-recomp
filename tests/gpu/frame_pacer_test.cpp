// Present timing for DLSS FG (runtime/gpu/frame_pacer.h). CPU only, hand-built instants.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "gpu/frame_pacer.h"

namespace {
using namespace std::chrono;
using gpu::DisplayedFrame;
using gpu::FramePacer;
using gpu::PacerClock;

PacerClock::time_point T0() { return PacerClock::time_point{} + seconds(100); }
double Ms(PacerClock::duration d) { return duration<double, std::milli>(d).count(); }

// Sequence R G R G ... with the generated one at `frac` of the interval (1 us ticks), consecutive present counts.
std::vector<DisplayedFrame> Sequence(int reals, double frac) {
  std::vector<DisplayedFrame> v;
  uint32_t n = 0;
  for (int i = 0; i < reals; ++i) {
    v.push_back({false, n++, int64_t(i) * 33000});
    if (i + 1 < reals) v.push_back({true, n++, int64_t(i) * 33000 + int64_t(33000 * frac)});
  }
  return v;
}
}  // namespace

TEST_CASE("generated immediately, real at half interval", "[frame_pacer]") {
  FramePacer p;
  std::vector<gpu::PacedPresent> out;
  for (int i = 0; i < 11; ++i) out = p.Schedule(true, false, T0() + milliseconds(33 * i));
  const auto ready = T0() + milliseconds(330);
  REQUIRE(out.size() == 2);
  CHECK(out[0].generated);
  CHECK(out[0].at == ready);
  CHECK_FALSE(out[1].generated);
  CHECK(Ms(out[1].at - ready) == Catch::Approx(16.5).margin(0.5));
}

TEST_CASE("without generated a single real immediately", "[frame_pacer]") {
  FramePacer p;
  p.Schedule(false, false, T0());
  auto out = p.Schedule(false, false, T0() + milliseconds(33));
  REQUIRE(out.size() == 1);
  CHECK_FALSE(out[0].generated);
  CHECK(out[0].at == T0() + milliseconds(33));
}

TEST_CASE("reset: T restarts from the last interval", "[frame_pacer]") {
  FramePacer p;
  for (int i = 0; i < 20; ++i) p.Schedule(true, false, T0() + milliseconds(33 * i));
  p.Schedule(true, true, T0() + milliseconds(33 * 19 + 16));
  CHECK(Ms(p.Interval()) == Catch::Approx(16.0).margin(0.01));
}

TEST_CASE("no present overtakes the previous one", "[frame_pacer]") {
  FramePacer p;
  std::vector<gpu::PacedPresent> all;
  // ready early (40 ms after 33 ms) and then irregular, also without generated.
  const int ms[] = {0, 33, 40, 66, 67, 100, 101, 102};
  for (int i = 0; i < 8; ++i)
    for (auto& x : p.Schedule(i != 5, false, T0() + milliseconds(ms[i]))) all.push_back(x);
  for (size_t i = 1; i < all.size(); ++i) CHECK(all[i].at >= all[i - 1].at + milliseconds(1));
}

TEST_CASE("T follows the switch from 33 to 16 ms", "[frame_pacer]") {
  FramePacer p;
  auto t = T0();
  for (int i = 0; i < 20; ++i, t += milliseconds(33)) p.Schedule(true, false, t);
  for (int i = 0; i < 30; ++i, t += milliseconds(16)) p.Schedule(true, false, t);
  CHECK(Ms(p.Interval()) == Catch::Approx(16.0).margin(1.0));
}

TEST_CASE("P: regular sequence 50, generated at 3/4 -> 25", "[frame_pacer]") {
  auto a = Sequence(12, 0.5);
  auto b = Sequence(12, 0.75);
  CHECK(*gpu::MedianHalfIntervalPercent(a) == Catch::Approx(50.f).margin(0.1));
  CHECK(*gpu::MedianHalfIntervalPercent(b) == Catch::Approx(25.f).margin(0.1));
}

TEST_CASE("P: fewer than 10 triples -> nullopt", "[frame_pacer]") {
  auto v = Sequence(10, 0.5);  // 9 triples
  CHECK_FALSE(gpu::MedianHalfIntervalPercent(v).has_value());
}

TEST_CASE("P: consecutive reals without generated make no triple", "[frame_pacer]") {
  std::vector<DisplayedFrame> v;
  for (uint32_t i = 0; i < 40; ++i) v.push_back({false, i, int64_t(i) * 33000});
  CHECK_FALSE(gpu::MedianHalfIntervalPercent(v).has_value());
}

TEST_CASE("P: a present_count gap breaks the triple", "[frame_pacer]") {
  auto v = Sequence(11, 0.5);  // 10 triples
  for (size_t i = 3; i < v.size(); ++i) v[i].present_count += 1;  // gap between v[2] and v[3]
  // the triple (v2,v3,v4) crosses the gap: 9 remain
  CHECK_FALSE(gpu::MedianHalfIntervalPercent(v).has_value());
}
