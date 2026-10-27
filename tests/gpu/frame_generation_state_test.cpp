// Per-frame state of frame generation (runtime/gpu/frame_generation_state.h). CPU only.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gpu/frame_generation_state.h"

TEST_CASE("FrameGenState: frameID, generation and reset", "[frame_generation]") {
  using namespace std::chrono;
  const auto t0 = steady_clock::now();
  auto ms = [&](int n) { return t0 + milliseconds(n); };
  gpu::FrameGenState s;
  auto f = s.Next(true, false, t0);
  CHECK((f.frame_id == 1 && !f.generate && !f.reset));
  f = s.Next(true, true, ms(10));
  CHECK((f.frame_id == 2 && f.generate && f.reset));
  CHECK(f.frame_ms == Catch::Approx(1000.0f / 60));
  f = s.Next(true, true, ms(25));
  CHECK((f.frame_id == 3 && f.generate && !f.reset));
  CHECK(f.frame_ms == Catch::Approx(15));
  f = s.Next(true, false, ms(40));
  CHECK((f.frame_id == 4 && !f.generate && !f.reset));
  f = s.Next(true, true, ms(50));
  CHECK((f.frame_id == 5 && f.generate && f.reset));
  f = s.Next(false, true, ms(60));
  CHECK((f.frame_id == 6 && !f.generate && !f.reset));
  f = s.Next(true, true, ms(70));
  CHECK((f.frame_id == 7 && f.generate && f.reset));
}

TEST_CASE("FrameGenState: Generating with 30-frame hysteresis", "[frame_generation]") {
  const auto t = std::chrono::steady_clock::now();
  gpu::FrameGenState s;
  CHECK_FALSE(s.Generating());
  s.Next(true, true, t);
  CHECK(s.Generating());
  for (int i = 0; i < 29; ++i) s.Next(true, false, t);
  CHECK(s.Generating());
  s.Next(true, false, t);
  CHECK_FALSE(s.Generating());
  s.Next(true, true, t);
  CHECK(s.Generating());
}
