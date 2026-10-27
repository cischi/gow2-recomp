// Timing logic of the present thread for DLSS FG and measurement of the on-screen
// cadence (P) for DLSS FG and FSR 3 FG. CPU only, no D3D.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gpu {

using PacerClock = std::chrono::steady_clock;

// Scanout instant of a displayed frame (DXGI GetFrameStatistics).
struct DisplayedFrame {
  bool generated;
  uint32_t present_count;  // DXGI_FRAME_STATISTICS::PresentCount
  int64_t sync_qpc;
};

// Earliest instant at which to Present.
struct PacedPresent {
  bool generated;
  PacerClock::time_point at;
};

class FramePacer {
 public:
  // A real frame ready (GPU finished) at instant `ready`; generated = the interpolated frame exists too.
  // reset = discard the history: T restarts from the interval between this `ready` and the previous one.
  // Returns the Presents to make, in order: [generated immediately, real at generated + T/2] or [real immediately].
  std::vector<PacedPresent> Schedule(bool generated, bool reset, PacerClock::time_point ready);
  // T: exponential moving average (alpha 0.1) of the interval between consecutive real frames.
  PacerClock::duration Interval() const { return interval_; }

 private:
  PacerClock::duration interval_ = std::chrono::microseconds(33333);
  std::optional<PacerClock::time_point> prev_ready_;
  std::optional<PacerClock::time_point> last_real_;
  bool measured_once_ = false;
};

// Median of P in percent: for each triple of previous real R0, generated G, real R1 consecutive in the sequence
// (and with consecutive PresentCount, otherwise a sampling gap would give false triples),
// P = (R1 - G) / (R1 - R0) * 100. nullopt if fewer than 10 triples.
std::optional<float> MedianHalfIntervalPercent(std::span<const DisplayedFrame> frames);

}  // namespace gpu
