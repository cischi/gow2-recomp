// Xenia's gamma ramp (IssueSwap's apply_gamma_table/apply_gamma_pwl): 256-entry table (DC_LUT_30_COLOR, red
// in bits 20:29) or 128-entry-per-channel PWL (DC_LUT_PWL_DATA, base in the low 16 bits). The includer declares first
// Buffer<uint> ramp_buf (256 table entries, then 384 PWL entries).
float3 Ramp(float3 c, bool pwl) {
  float3 o;
  if (pwl) {
    const uint3 i = uint3(saturate(c) * 1023.0 + 0.5);
    [unroll] for (uint ch = 0; ch < 3; ++ch) {
      const uint v = ramp_buf[256 + (i[ch] >> 3) * 3 + ch];
      o[ch] = saturate((float(v & 0xFFFF) + float((i[ch] & 7) * (v >> 16)) * (1.0 / 8.0)) * (1.0 / (64.0 * 1023.0)));
    }
  } else {
    const uint3 i = uint3(saturate(c) * 255.0 + 0.5);
    o = float3(ramp_buf[i.r] >> 20 & 1023, ramp_buf[i.g] >> 10 & 1023, ramp_buf[i.b] & 1023) * (1.0 / 1023.0);
  }
  return o;
}
