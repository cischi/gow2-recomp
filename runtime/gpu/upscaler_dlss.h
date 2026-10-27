// Jitter for DLSS: the NVIDIA guide asks for a Halton sequence
// (2, 3) with at least 8 * (output / render)^2 phases; FSR gets it from its SDK, DLSS does not.
#pragma once

#include <cstdint>

#include "settings.h"

namespace gpu {

// ceil(8 * (output_w / render_w)^2), at least 8.
uint32_t DlssJitterPhaseCount(uint32_t render_w, uint32_t output_w);
// Halton(index % phases + 1) in base 2 and 3, minus 0.5: render pixels in [-0.5, 0.5).
void DlssJitterOffset(uint32_t index, uint32_t phases, float& x, float& y);

// NVSDK_NGX_DLSS_Hint_Render_Preset_* value of the bridge: default 0, K 11, M 13, L 12.
uint32_t NgxPreset(settings::DlssPreset preset);

}  // namespace gpu
