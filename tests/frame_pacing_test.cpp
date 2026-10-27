// Pacing per value, presenter limit and frame limiter.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "frame_pacing.h"

using Catch::Approx;
using namespace std::chrono_literals;
using settings::FrameRate;
using settings::Vrr;
namespace fp = frame_pacing;

TEST_CASE("frame_pacing: PacingFor follows the pacing table", "[frame_pacing]") {
  struct Row {
    FrameRate rate;
    bool patched;
    uint32_t interval;
    double hz;
  };
  const Row rows[] = {{FrameRate::k30, false, 2, 60},   {FrameRate::k60, true, 1, 60},
                      {FrameRate::k120, true, 1, 120},  {FrameRate::k144, true, 1, 144},
                      {FrameRate::kMonitor, true, 0, 60}, {FrameRate::kUnlimited, true, 0, 60}};
  for (const Row& r : rows) {
    const fp::Pacing p = fp::PacingFor(r.rate);
    INFO(settings::FrameRateName(r.rate));
    CHECK(p.patched == r.patched);
    CHECK(p.interval == r.interval);
    CHECK(p.vblank_hz == r.hz);
  }
}

TEST_CASE("frame_pacing: VrrCap", "[frame_pacing]") {
  CHECK(fp::VrrCap(144) == Approx(138.24));
  CHECK(fp::VrrCap(120) == Approx(116.0));
  CHECK(fp::VrrCap(60) == Approx(59.0));
  CHECK(fp::VrrCap(165) == Approx(157.4375));
}

TEST_CASE("frame_pacing: LimiterHz", "[frame_pacing]") {
  CHECK(fp::LimiterHz(FrameRate::k30, true, false, 165) == 0);
  CHECK(fp::LimiterHz(FrameRate::k120, true, false, 0) == 0);
  CHECK(fp::LimiterHz(FrameRate::kUnlimited, true, false, 165) == Approx(157.4375));
  CHECK(fp::LimiterHz(FrameRate::kUnlimited, false, false, 165) == 0);
  CHECK(fp::LimiterHz(FrameRate::k60, true, true, 165) == Approx(157.4375));
  CHECK(fp::LimiterHz(FrameRate::kMonitor, false, false, 165) == 165);
  CHECK(fp::LimiterHz(FrameRate::kMonitor, false, true, 165) == 0);
}

TEST_CASE("frame_pacing: VrrActive", "[frame_pacing]") {
  CHECK(fp::VrrActive(Vrr::kOn, std::nullopt));
  CHECK_FALSE(fp::VrrActive(Vrr::kOff, true));
  CHECK_FALSE(fp::VrrActive(Vrr::kAuto, std::nullopt));
  CHECK_FALSE(fp::VrrActive(Vrr::kAuto, false));
  CHECK(fp::VrrActive(Vrr::kAuto, true));
}

TEST_CASE("frame_pacing: VblankPeriod", "[frame_pacing]") {
  CHECK(fp::VblankPeriod(60) == 16667us);
  CHECK(fp::VblankPeriod(120) == 8333us);
  CHECK(fp::VblankPeriod(144) == 6944us);
}

TEST_CASE("frame_pacing: FrameLimiter at 200 Hz, no catch-up after a slow frame", "[frame_pacing]") {
  using Clock = std::chrono::steady_clock;
  fp::FrameLimiter lim;
  lim.Wait(0);  // hz <= 0: returns immediately
  Clock::time_point t[51];
  for (auto& x : t) {
    lim.Wait(200);
    x = Clock::now();
  }
  for (int i = 1; i < 51; ++i)
    CHECK(std::chrono::duration<double, std::milli>(t[i] - t[i - 1]).count() >= 4.9);
  CHECK(std::chrono::duration<double, std::milli>(t[50] - t[0]).count() <= 50 * 5.0 * 1.2);

  std::this_thread::sleep_for(30ms);
  const auto a = Clock::now();
  lim.Wait(200);
  const auto b = Clock::now();
  lim.Wait(200);
  const auto c = Clock::now();
  CHECK(b - a < 2ms);
  CHECK(std::chrono::duration<double, std::milli>(c - b).count() >= 4.9);
}

TEST_CASE("frame_pacing: SyncPoller queries in the constructor and then on another thread, Get does not wait",
          "[frame_pacing]") {
  std::atomic<int> calls{0};
  std::atomic<std::thread::id> last_thread{};
  fp::SyncPoller poller([&] {
    last_thread = std::this_thread::get_id();
    const int n = ++calls;
    if (n > 1) std::this_thread::sleep_for(20ms);  // like NVAPI, slow
    return plat::DisplaySync{uint32_t(100 + n), {}, "", 0};
  });
  CHECK(calls == 1);  // first query is synchronous
  CHECK(last_thread.load() == std::this_thread::get_id());
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(poller.Get().refresh_hz == 101);  // the last value, without waiting
  CHECK(std::chrono::steady_clock::now() - t0 < 5ms);
  for (int i = 0; i < 200 && calls < 2; ++i) std::this_thread::sleep_for(5ms);
  std::this_thread::sleep_for(40ms);  // the second query finishes and is published
  CHECK(calls == 2);
  CHECK(last_thread.load() != std::this_thread::get_id());
  CHECK(poller.Get().refresh_hz == 102);
}

TEST_CASE("frame_pacing: FrameGenLimiterHz", "[frame_pacing]") {
  CHECK(fp::FrameGenLimiterHz(FrameRate::k60, 165) == Approx(77.9315625));
  CHECK(fp::FrameGenLimiterHz(FrameRate::kUnlimited, 165) == Approx(77.9315625));
  CHECK(fp::FrameGenLimiterHz(FrameRate::k144, 60) == Approx(29.205));
  CHECK(fp::FrameGenLimiterHz(FrameRate::k30, 165) == 0);
  CHECK(fp::FrameGenLimiterHz(FrameRate::k60, 0) == 0);
}
