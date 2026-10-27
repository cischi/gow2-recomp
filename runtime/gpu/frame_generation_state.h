// Per-frame state of frame generation: frame id, whether to generate, history reset and frame time.
#pragma once

#include <chrono>
#include <cstdint>

namespace gpu {

struct FrameGenFrame {
  uint64_t frame_id;  // +1 on every Present, starting from the first (1)
  bool generate;      // generate for this frame: enabled && has_inputs
  bool reset;         // first frame that generates after one that did not (or the very first)
  float frame_ms;     // ms since the previous generating frame; 1000/60 if reset
  bool operator==(const FrameGenFrame&) const = default;
};

class FrameGenState {
 public:
  // One Present. enabled: option turned on (hot); has_inputs: the frame capture is usable.
  FrameGenFrame Next(bool enabled, bool has_inputs, std::chrono::steady_clock::time_point now);
  static constexpr uint32_t kGeneratingWindow = 30;
  // True if one of the last kGeneratingWindow Next calls generated (halved real-frame limit, spec closing #1).
  bool Generating() const { return since_generated_ < kGeneratingWindow; }

 private:
  uint64_t frame_id_ = 0;
  bool prev_generated_ = false;
  uint32_t since_generated_ = kGeneratingWindow;  // Next calls since the last one that generated
  std::chrono::steady_clock::time_point prev_time_{};
};

}  // namespace gpu
