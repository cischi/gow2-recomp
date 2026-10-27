// Pacing per frame_rate value, presenter limit and frame limiter.
#pragma once

#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "platform/platform.h"
#include "settings.h"

namespace frame_pacing {

struct Pacing {
  bool patched;       // false: 30, no internal patch
  uint32_t interval;  // value for game_patches::SetPresentInterval (0 or 1); 2 if !patched
  double vblank_hz;   // 60, 120 or 144
};
Pacing PacingFor(settings::FrameRate);

// Limit below the refresh rate that keeps VRR inside its window: refresh - refresh^2/3600.
double VrrCap(double refresh_hz);

// Real frames with frame generation: VrrCap(refresh_hz) / 2 * 0.99 (the proxy presents double); 0 with k30 or refresh_hz == 0.
double FrameGenLimiterHz(settings::FrameRate, uint32_t refresh_hz);

// Presenter limit in fps; 0 = none.
double LimiterHz(settings::FrameRate, bool vrr_active, bool vsync, uint32_t refresh_hz);

// Auto: the NVAPI result, false if unavailable.
bool VrrActive(settings::Vrr, std::optional<bool> nvapi_vrr);

// round(1e6 / hz).
std::chrono::microseconds VblankPeriod(double hz);

// One instance per presenter, a single thread. No catch-up after a slow frame:
// every interval lasts at least one period.
class FrameLimiter {
 public:
  void Wait(double hz);  // hz <= 0: returns immediately

 private:
  std::chrono::steady_clock::time_point next_{};
};

// Monitor query outside the Present thread: NVAPI costs 4-5 ms per call (measured in game),
// beyond the millisecond granted to the presenter. The first is synchronous in the constructor; every Get returns the last
// result without waiting and asks the thread for a new query, published for the next Get.
class SyncPoller {
 public:
  explicit SyncPoller(std::function<plat::DisplaySync()> query);
  ~SyncPoller();
  plat::DisplaySync Get();

 private:
  std::function<plat::DisplaySync()> query_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool requested_ = false, stop_ = false;
  plat::DisplaySync last_;
  std::thread thread_;  // last: starts with the members ready
};

}  // namespace frame_pacing
