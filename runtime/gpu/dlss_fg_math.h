// Camera matrices for DLSS frame generation, derived from the uber post constants.
// CPU only. NGX convention: row-major, "post-multiplication" (v' = v * M, m[r * 4 + c]).
#pragma once

#include <array>
#include <optional>

#include "gpu/upscale_passes.h"

namespace gpu {

using Mat4 = std::array<float, 16>;

// Current clip (X, Y, Z, W) -> previous-frame clip, reproduces upscale_motion_vectors.hlsl. Z is the game's
// non-linear depth (see the derivation in the .cpp).
Mat4 ClipToPrevClip(const UberPostConstants& k);

// nullopt if the matrix is singular.
std::optional<Mat4> Invert4x4(const Mat4& m);

// D3D reversed-depth projection with infinite far plane, like the game's: z_ndc = near / z_view.
// far_z is ignored (infinite far), kept in the signature for callers that pass the camera parameters.
Mat4 ViewToClip(float fov_y, float aspect, float near_z, float far_z);

}  // namespace gpu
