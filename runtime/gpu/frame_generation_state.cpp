#include "gpu/frame_generation_state.h"

#include <algorithm>

namespace gpu {

FrameGenFrame FrameGenState::Next(bool enabled, bool has_inputs, std::chrono::steady_clock::time_point now) {
  FrameGenFrame f{++frame_id_, enabled && has_inputs, false, 1000.0f / 60};
  if (f.generate) {
    f.reset = !prev_generated_;
    if (!f.reset) f.frame_ms = std::chrono::duration<float, std::milli>(now - prev_time_).count();
    prev_time_ = now;
  }
  prev_generated_ = f.generate;
  since_generated_ = f.generate ? 0 : std::min(since_generated_ + 1, kGeneratingWindow);
  return f;
}

}  // namespace gpu
