// like hdr::SrgbToLinear
float3 SrgbToLinear(float3 v) {
  v = saturate(v);
  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
