#include "hdr_math.h"

#include <algorithm>
#include <cmath>

namespace hdr {
namespace {

float Sat(float x) { return std::min(std::max(x, 0.0f), 1.0f); }

constexpr double kM1 = 0.1593017578125, kM2 = 78.84375, kC1 = 0.8359375, kC2 = 18.8515625, kC3 = 18.6875;

// The ucode's pow: exp2(log2(max(|x|, c254z)) * e).
float UPow(float x, float e, float c254z) { return std::exp2(std::log2(std::max(std::fabs(x), c254z)) * e); }

}  // namespace

float SrgbToLinear(float srgb) {
  const float c = Sat(srgb);
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float PqFromNits(float nits) {
  const double y = std::pow(std::clamp(static_cast<double>(nits) / 10000.0, 0.0, 1.0), kM1);
  return static_cast<float>(std::pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2));
}

float NitsFromPq(float pq) {
  const double q = std::pow(std::clamp(static_cast<double>(pq), 0.0, 1.0), 1.0 / kM2);
  const double d = kC2 - kC3 * q;
  return static_cast<float>(10000.0 * std::pow(std::max(q - kC1, 0.0) / d, 1.0 / kM1));
}

Rgb Bt709ToBt2020(Rgb l) {
  return {0.627403896f * l.r + 0.329283039f * l.g + 0.043313065f * l.b,
          0.069097289f * l.r + 0.919540395f * l.g + 0.011362316f * l.b,
          0.016391439f * l.r + 0.088013308f * l.g + 0.895595253f * l.b};
}

float Shoulder(float l, float p) {
  if (l <= 1.0f) return l;
  if (p <= 1.0f) return 1.0f;
  const float t = l - 1.0f;
  return 1.0f + t * (p - 1.0f) / (t + p - 1.0f);
}

UberTonemapOut UberTonemap(Rgb pre, const UberTonemapConstants& k) {
  const float in[3] = {pre.r, pre.g, pre.b};
  float out[2][3];
  for (int unclamped = 0; unclamped < 2; ++unclamped) {
    float x[3];
    for (int i = 0; i < 3; ++i) {
      const float d = in[i] - k.c5[i];
      x[i] = UPow((unclamped ? std::max(d, 0.0f) : Sat(d)) * k.c6[i], k.c7[i], k.c254z);
    }
    const float dot = x[0] * k.c8[0] + x[1] * k.c8[1] + x[2] * k.c8[2];
    for (int i = 0; i < 3; ++i) {
      const float y = (x[i] * k.c5[3] + k.c10[i] + dot) * k.c9[i];
      out[unclamped][i] = unclamped ? std::max(y, 0.0f) : Sat(y);
    }
  }
  return {{out[0][0], out[0][1], out[0][2]}, {out[1][0], out[1][1], out[1][2]}};
}

float HudMask(Rgb guest, Rgb ramp) {
  const float d = std::max({std::fabs(guest.r - ramp.r), std::fabs(guest.g - ramp.g), std::fabs(guest.b - ramp.b)});
  const float t = Sat((d - 4.0f / 1023.0f) / (60.0f / 1023.0f));
  return t * t * (3.0f - 2.0f * t);
}

Rgb Residual(Rgb l_sdr, Rgb y, Rgb y_unclamped, float c9w, float p, float m) {
  const float ls[3] = {l_sdr.r, l_sdr.g, l_sdr.b}, yc[3] = {y.r, y.g, y.b},
              yu[3] = {y_unclamped.r, y_unclamped.g, y_unclamped.b};
  float o[3];
  for (int i = 0; i < 3; ++i) {
    const float g = std::max(yu[i] / std::max(yc[i], 1e-4f), 1.0f);
    const float l = Shoulder(ls[i] * std::pow(g, 2.2f * c9w), p);
    o[i] = std::max(l - ls[i], 0.0f) * (1.0f - m);
  }
  return {o[0], o[1], o[2]};
}

Rgb ComposePq(Rgb srgb, Rgb residual, float paper_white_nits) {
  const Rgb l = Bt709ToBt2020({SrgbToLinear(srgb.r) + residual.r, SrgbToLinear(srgb.g) + residual.g,
                               SrgbToLinear(srgb.b) + residual.b});
  return {PqFromNits(l.r * paper_white_nits), PqFromNits(l.g * paper_white_nits), PqFromNits(l.b * paper_white_nits)};
}

Levels ChooseLevels(uint32_t peak_setting, uint32_t paper_white_setting, const Display& d) {
  float peak = 1000.0f, paper = 203.0f;
  if (peak_setting > 0) peak = static_cast<float>(peak_setting);
  else if (d.hdr && d.max_nits > 0) peak = d.max_nits;
  if (paper_white_setting > 0) paper = static_cast<float>(paper_white_setting);
  else if (d.hdr && d.sdr_white_nits > 0) paper = d.sdr_white_nits;
  return {peak, std::min(paper, peak)};
}

int ChooseOutput(const Rect& w, std::span<const Rect> outputs) {
  int best = -1;
  int64_t best_area = 0;
  for (size_t i = 0; i < outputs.size(); ++i) {
    const Rect& o = outputs[i];
    const int64_t iw = static_cast<int64_t>(std::min(w.right, o.right)) - std::max(w.left, o.left);
    const int64_t ih = static_cast<int64_t>(std::min(w.bottom, o.bottom)) - std::max(w.top, o.top);
    if (iw > 0 && ih > 0 && iw * ih > best_area) {
      best_area = iw * ih;
      best = static_cast<int>(i);
    }
  }
  return best;
}

}  // namespace hdr
