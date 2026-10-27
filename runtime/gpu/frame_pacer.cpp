#include "gpu/frame_pacer.h"

#include <algorithm>

namespace gpu {

std::vector<PacedPresent> FramePacer::Schedule(bool generated, bool reset, PacerClock::time_point ready) {
  using namespace std::chrono;
  if (prev_ready_ && ready > *prev_ready_) {
    const auto measured = ready - *prev_ready_;
    // Reset: discard the history. First interval (T still at its initial value): adopt it without averaging.
    interval_ = reset || !measured_once_ ? measured : interval_ + (measured - interval_) / 10;
    measured_once_ = true;
  }
  prev_ready_ = ready;

  // No present goes ahead of the previous one: at least 1 ms later (applies to generated and real).
  const auto at = last_real_ ? std::max(ready, *last_real_ + milliseconds(1)) : ready;
  std::vector<PacedPresent> out;
  if (generated) {
    out.push_back({true, at});
    last_real_ = at + std::max<PacerClock::duration>(interval_ / 2, milliseconds(1));
  } else {
    last_real_ = at;
  }
  out.push_back({false, *last_real_});
  return out;
}

std::optional<float> MedianHalfIntervalPercent(std::span<const DisplayedFrame> frames) {
  std::vector<float> p;
  for (size_t i = 0; i + 2 < frames.size(); ++i) {
    const auto &r0 = frames[i], &g = frames[i + 1], &r1 = frames[i + 2];
    if (r0.generated || !g.generated || r1.generated) continue;
    if (g.present_count != r0.present_count + 1 || r1.present_count != g.present_count + 1) continue;
    if (r1.sync_qpc <= r0.sync_qpc) continue;
    p.push_back(float(r1.sync_qpc - g.sync_qpc) / float(r1.sync_qpc - r0.sync_qpc) * 100.f);
  }
  if (p.size() < 10) return std::nullopt;
  std::nth_element(p.begin(), p.begin() + p.size() / 2, p.end());
  float m = p[p.size() / 2];
  if (p.size() % 2 == 0) m = (m + *std::max_element(p.begin(), p.begin() + p.size() / 2)) / 2;
  return m;
}

}  // namespace gpu
