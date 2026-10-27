// SPDX-License-Identifier: MIT (see LICENSE.txt in this folder)
//
// DLSS mode selection from the output/render ratio: pure function, no NVIDIA types (testable without the SDK).
#pragma once

#include <stdint.h>

namespace gow2_dlss {

enum class Quality { kDlaa, kMaxQuality, kBalanced, kMaxPerf, kUltraPerf };

// Ratio <= 1 (render = output, or smaller output): DLAA. Then < 1.6 Quality, < 1.85 Balanced,
// < 2.5 Performance, otherwise Ultra Performance.
inline Quality QualityFor(uint32_t render_w, uint32_t output_w) {
  if (render_w == 0 || output_w <= render_w) return Quality::kDlaa;
  const float ratio = float(output_w) / float(render_w);
  if (ratio >= 2.5f) return Quality::kUltraPerf;
  if (ratio >= 1.85f) return Quality::kMaxPerf;
  if (ratio >= 1.6f) return Quality::kBalanced;
  return Quality::kMaxQuality;
}

}  // namespace gow2_dlss
