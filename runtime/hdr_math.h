// HDR10 output math: the hdr_*.hlsl shaders mirror it,
// each function has a namesake carrying the comment "as hdr::X". No Windows or D3D12.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace hdr {

struct Rgb {
  float r, g, b;
  bool operator==(const Rgb&) const = default;
};

float SrgbToLinear(float srgb);  // IEC 61966-2-1, input saturated
float PqFromNits(float nits);    // ST.2084, nits/10000 saturated
float NitsFromPq(float pq);      // inverse
Rgb Bt709ToBt2020(Rgb linear);   // BT.2087
// Identity up to 1, then towards peak_over_paper with derivative 1 at 1; with p <= 1, min(l, 1).
float Shoulder(float l, float peak_over_paper);

struct UberTonemapConstants {
  float c5[4], c6[4], c7[4], c8[4], c9[4], c10[4];
  float c254z;
};
struct UberTonemapOut {
  Rgb y, y_unclamped;
};
// Lines 70-84 of the uber post; pre = color before "- c5.xyz". y_unclamped without the two upper clamps.
UberTonemapOut UberTonemap(Rgb pre, const UberTonemapConstants& k);
// smoothstep(4/1023, 64/1023, max |guest - ramp_prehud|)
float HudMask(Rgb guest, Rgb ramp_prehud);
// Extra light compared with SDR, in paper-white units, per channel.
Rgb Residual(Rgb l_sdr, Rgb y, Rgb y_unclamped, float c9w, float peak_over_paper, float hud_mask);
// PQ codes 0..1, BT.2020.
Rgb ComposePq(Rgb srgb, Rgb residual, float paper_white_nits);

struct Display {
  bool hdr = false;
  float max_nits = 0, sdr_white_nits = 0;
  std::string name;
};
struct Levels {
  float peak_nits, paper_white_nits;
  bool operator==(const Levels&) const = default;
};
Levels ChooseLevels(uint32_t peak_setting, uint32_t paper_white_setting, const Display& display);

struct Rect {
  int32_t left, top, right, bottom;
};
// Index of the output with the largest intersection with the window; -1 if none.
int ChooseOutput(const Rect& window, std::span<const Rect> outputs);

}  // namespace hdr
