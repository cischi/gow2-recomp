#include "frame_pacing.h"

#include <cmath>

#include "platform/platform.h"

namespace frame_pacing {

Pacing PacingFor(settings::FrameRate rate) {
  using settings::FrameRate;
  switch (rate) {
    case FrameRate::k60: return {true, 1, 60};
    case FrameRate::k120: return {true, 1, 120};
    case FrameRate::k144: return {true, 1, 144};
    case FrameRate::kMonitor:
    case FrameRate::kUnlimited: return {true, 0, 60};
    case FrameRate::k30: break;
  }
  return {false, 2, 60};
}

double VrrCap(double refresh_hz) { return refresh_hz - refresh_hz * refresh_hz / 3600.0; }

double FrameGenLimiterHz(settings::FrameRate rate, uint32_t refresh_hz) {
  if (rate == settings::FrameRate::k30 || refresh_hz == 0) return 0;
  // 1% below the proxy's pace (~VrrCap/2): above it, the proxy makes Present wait and the real frames queue up (spike: +9 ms).
  return VrrCap(refresh_hz) / 2 * 0.99;
}

double LimiterHz(settings::FrameRate rate, bool vrr_active, bool vsync, uint32_t refresh_hz) {
  if (rate == settings::FrameRate::k30 || refresh_hz == 0) return 0;
  if (vrr_active) return VrrCap(refresh_hz);
  if (rate == settings::FrameRate::kMonitor && !vsync) return refresh_hz;
  return 0;
}

bool VrrActive(settings::Vrr vrr, std::optional<bool> nvapi_vrr) {
  if (vrr == settings::Vrr::kAuto) return nvapi_vrr.value_or(false);
  return vrr == settings::Vrr::kOn;
}

std::chrono::microseconds VblankPeriod(double hz) {
  return std::chrono::microseconds(int64_t(std::llround(1e6 / hz)));
}

void FrameLimiter::Wait(double hz) {
  if (hz <= 0) return;
  using Clock = std::chrono::steady_clock;
  auto now = Clock::now();
  if (now < next_) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(next_ - now).count();
    if (us > 1500) plat::SleepPrecise(uint32_t(us - 1000));
    while ((now = Clock::now()) < next_) {
    }
  }
  next_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
}

SyncPoller::SyncPoller(std::function<plat::DisplaySync()> query) : query_(std::move(query)), last_(query_()) {
  thread_ = std::thread([this] {
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return requested_ || stop_; });
      if (stop_) return;
      requested_ = false;
      lock.unlock();
      plat::DisplaySync d = query_();
      lock.lock();
      last_ = std::move(d);
    }
  });
}

SyncPoller::~SyncPoller() {
  {
    std::lock_guard g(mutex_);
    stop_ = true;
  }
  cv_.notify_one();
  thread_.join();
}

plat::DisplaySync SyncPoller::Get() {
  std::lock_guard g(mutex_);
  requested_ = true;
  cv_.notify_one();
  return last_;
}

}  // namespace frame_pacing
