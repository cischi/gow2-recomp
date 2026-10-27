// HDR10 output: presented SDR image (sRGB) plus the residual from hdr_reconstruct.hlsl (bilinear inside the image
// rectangle), BT.709 -> BT.2020, PQ (ST.2084) in nits / 10000. Where pre_overlay differs from the SDR (runtime
// overlay on top of the image) no residual. Numeric reference: runtime/hdr_math.cpp (functions of the same name).
#define RS "RootConstants(num32BitConstants=8, b0), DescriptorTable(SRV(t0, numDescriptors=3)), " \
  "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

cbuffer K : register(b0) {
  uint4 rect;           // image in the back buffer: left, top, right, bottom
  float paper_white;    // nits
  uint has_residual;    // 0: t_resid not read
  uint has_pre_overlay; // 0: t_pre_overlay not read
  uint pad;
};
Texture2D<float4> t_sdr : register(t0);
Texture2D<float4> t_pre_overlay : register(t1);
Texture2D<float4> t_resid : register(t2);
SamplerState s_lin : register(s0);

#include "srgb.hlsli"

// like hdr::PqFromNits
float3 Pq(float3 nits) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const float3 y = pow(saturate(nits / 10000.0), m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// like hdr::Bt709ToBt2020 (BT.2087)
float3 Bt709ToBt2020(float3 l) {
  const float3x3 m = {0.627403896, 0.329283039, 0.043313065,
                      0.069097289, 0.919540395, 0.011362316,
                      0.016391439, 0.088013308, 0.895595253};
  return mul(m, l);
}

float4 vs_main(uint i : SV_VertexID) : SV_Position {
  const float2 p = float2((i << 1) & 2, i & 2);
  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

[RootSignature(RS)]
float4 ps_main(float4 pos : SV_Position) : SV_Target {
  const int2 p = int2(pos.xy);
  const float3 sdr = t_sdr.Load(int3(p, 0)).rgb;
  float3 l = SrgbToLinear(sdr);
  const bool overlay = has_pre_overlay && any(sdr != t_pre_overlay.Load(int3(p, 0)).rgb);
  if (has_residual && !overlay && all(uint2(p) >= rect.xy) && all(uint2(p) < rect.zw)) {
    const float2 uv = (float2(uint2(p) - rect.xy) + 0.5) / float2(rect.zw - rect.xy);
    l += t_resid.SampleLevel(s_lin, uv, 0).rgb;
  }
  return float4(Pq(Bt709ToBt2020(l) * paper_white), 1.0);
}
