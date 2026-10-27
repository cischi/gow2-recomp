// See dlss_fg_math.h.
#include "gpu/dlss_fg_math.h"

#include <cmath>

namespace gpu {

Mat4 ClipToPrevClip(const UberPostConstants& k) {
  // The motion vector shader: prev = c13.xyw*X + c14.xyw*Y + c15.xyw*W + c16.xyw, with (X, Y, W) = (s*d, d).
  // The constant is rewritten with d = 1/(z*c2.z - c2.w), i.e. 1 = c2.z*Z - c2.w*W. Columns x, y, w of M.
  const float* x = k.c13;
  const float* y = k.c14;
  const float* z = k.c16;  // multiplied by c2.z
  const float* w = k.c15;
  const float a = k.c2[2], b = k.c2[3];
  Mat4 m{};
  auto row = [&](int r, const float* src, float scale, const float* sub, float sub_scale) {
    m[r * 4 + 0] = scale * src[0] - sub_scale * sub[0];
    m[r * 4 + 1] = scale * src[1] - sub_scale * sub[1];
    m[r * 4 + 3] = scale * src[3] - sub_scale * sub[3];
  };
  // The shader components are x, y, w: indices 0, 1, 3. Rows X and Y have no constant term.
  row(0, x, 1.0f, k.c16, 0.0f);
  row(1, y, 1.0f, k.c16, 0.0f);
  row(2, z, a, k.c16, 0.0f);
  row(3, w, 1.0f, z, b);
  // Column z: z_prev = (c2.w * w_prev + 1) / c2.z, so (c2.w/c2.z) * column w, plus 1 in row Z and minus
  // c2.w/c2.z in row W.
  const float q = b / a;
  for (int r = 0; r < 4; ++r) m[r * 4 + 2] = q * m[r * 4 + 3];
  m[2 * 4 + 2] += 1.0f;
  m[3 * 4 + 2] -= q;
  return m;
}

std::optional<Mat4> Invert4x4(const Mat4& m) {
  // Gauss-Jordan with partial pivoting, in double.
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][4 + c] = r == c ? 1.0 : 0.0;
    }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return std::nullopt;
    for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
    const double inv = 1.0 / a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  Mat4 out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][4 + c]);
  return out;
}

Mat4 ViewToClip(float fov_y, float aspect, float near_z, float /*far_z*/) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  Mat4 m{};
  m[0] = f / aspect;
  m[5] = f;
  m[2 * 4 + 3] = 1.0f;       // w_clip = z_view
  m[3 * 4 + 2] = near_z;     // z_clip = near (z_ndc = near / z_view)
  return m;
}

}  // namespace gpu
