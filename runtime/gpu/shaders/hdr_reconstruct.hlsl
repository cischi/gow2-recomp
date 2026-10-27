// HDR10 output: light residual relative to the presented SDR, at
// render resolution. Redoes the UE3 uber post (ucode B6B6DD4E7D91F3D9, instructions 8-84) from the captured textures:
// pre = color after motion blur, DoF and bloom; y = the game's chain, yh = the same without the upper clamps. The
// residual is the SDR light extended with yh / y and pushed toward the peak, minus the SDR light, zero under the HUD.
// Numeric reference: runtime/hdr_math.cpp (functions of the same name).
#define RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=7), UAV(u0)), " \
  "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP), " \
  "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

cbuffer K : register(b0) {
  float4 c[19];
  float4 c253, c254, c255;
  uint4 filt;  // xyz: tf1, tf2, tf3 with linear filter; w: PWL ramp
  float4 scl;  // xyz: 2^exp_adjust of tf1, tf2, tf3; w: peak / paper white
};
Texture2D<float> t_depth : register(t0);
Texture2D<float4> t_scene : register(t1);
Texture2D<float4> t_bloom : register(t2);
Texture2D<float4> t_vel : register(t3);
Texture2D<float4> t_prehud : register(t4);
Texture2D<float4> t_guest : register(t5);
Buffer<uint> ramp_buf : register(t6);
RWTexture2D<float4> o_resid : register(u0);
SamplerState s_point : register(s0);
SamplerState s_lin : register(s1);

#include "srgb.hlsli"
#include "gamma_ramp.hlsli"

// The ucode's Pow, like UPow in hdr_math.cpp
float3 Pow(float3 x, float3 e) { return exp2(log2(max(abs(x), c254.z)) * e); }

// like hdr::Shoulder
float3 Shoulder(float3 l, float p) {
  const float3 t = max(l - 1.0, 0.0);
  return l <= 1.0 ? l : (p <= 1.0 ? 1.0 : 1.0 + t * (p - 1.0) / (t + p - 1.0));
}

// like hdr::UberTonemap (instructions 70-84); pre before "- c5.xyz"
void UberTonemap(float3 pre, out float3 y, out float3 yh) {
  float3 x = Pow(saturate(pre - c[5].xyz) * c[6].xyz, c[7].xyz);
  y = saturate((x * c[5].w + c[10].xyz + dot(x, c[8].xyz)) * c[9].xyz);
  x = Pow(max(pre - c[5].xyz, 0.0) * c[6].xyz, c[7].xyz);
  yh = max((x * c[5].w + c[10].xyz + dot(x, c[8].xyz)) * c[9].xyz, 0.0);
}

// like hdr::HudMask
float HudMask(float3 guest, float3 ramp_prehud) {
  const float3 d = abs(guest - ramp_prehud);
  return smoothstep(4.0 / 1023.0, 64.0 / 1023.0, max(d.r, max(d.g, d.b)));
}

// like hdr::Residual
float3 Residual(float3 l_sdr, float3 y, float3 yh, float c9w, float peak_over_paper, float m) {
  const float3 g = max(yh / max(y, 1e-4), 1.0);
  const float3 l = Shoulder(l_sdr * pow(g, 2.2 * c9w), peak_over_paper);
  return max(l - l_sdr, 0.0) * (1.0 - m);
}

float4 Scene(float2 uv) {
  return (filt.x ? t_scene.SampleLevel(s_lin, uv, 0) : t_scene.SampleLevel(s_point, uv, 0)) * scl.x;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 size;
  t_prehud.GetDimensions(size.x, size.y);
  if (any(id.xy >= size)) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(size);  // r0.zw
  uint2 bsize;
  t_bloom.GetDimensions(bsize.x, bsize.y);
  // r1.xy: tf2 (bloom/DoF at 1/4) has a 1-texel border, the content is (bsize - 2) (measured on the spike dumps).
  const float2 uv2 = (uv * float2(bsize - 2) + 1.0) / float2(bsize);
  const float2 S = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);  // r0.xy (NDC)

  const float4 r3 = Scene(uv);  // 8
  const float2 v = (filt.z ? t_vel.SampleLevel(s_lin, uv, 0) : t_vel.SampleLevel(s_point, uv, 0)).xy * scl.z;  // 9
  const float z = t_depth.SampleLevel(s_point, uv, 0);  // 10
  const float lin_d = 1.0 / (z * c[2].z - c[2].w);  // 11-12, r2.y
  float2 n, d;  // r0.xy, r2.xz
  if (c253.z == 0.0 && v.x > 0.0) {  // 12-14: dynamic object
    n = v * c254.x + c255.w;
    d = n * c[18].xy;
  } else {  // 15-29
    float dd = min(lin_d, c253.y);
    if (c254.y > dd) dd = c253.y;
    const float3 p = dd * (c[15].xyw + S.y * c[14].xyw + S.x * c[13].xyw) + c[16].xyw;
    const float2 m = (S - p.xy / p.z).yx * c[17].yx;  // 24-26: r0.x = dy*c17.y, r0.y = dx*c17.x
    d = float2(min(max(m.y, -c[17].z), c[17].z), min(max(m.x, -c[17].w), c[17].w));  // 27-28
    n = d / c[17].zw;  // 29
  }
  const float2 ab = (dot(n, n) + c253.z) * c255.xy;  // 30-31
  const float4 lo = c[11].xyxy, hi = c[11].zwzw;
  const float4 pa = clamp(ab.xxyy * d.xyxy + uv.xyxy, lo, hi);  // 32, 35-36 (r0)
  const float4 ma = clamp(-ab.xxyy * d.xyxy + uv.xyxy, lo, hi);  // 33-34, 37 (r5)
  const float4 r1 = (filt.y ? t_bloom.SampleLevel(s_lin, uv2, 0) : t_bloom.SampleLevel(s_point, uv2, 0)) * scl.y;  // 38
  const float4 r4 = Scene(ma.zw);  // 39
  const float4 r5 = Scene(ma.xy);  // 40
  const float4 r7 = Scene(pa.zw);  // 41
  const float4 r8 = Scene(pa.xy);  // 42

  // 43-67: depth of field and motion blur weights (CoC from tf1's alpha).
  const float cC = c255.z > r3.w ? c253.z : abs(r3.w * c[12].x + c[12].y);
  const float dz = lin_d - c[3].x;
  const float sel = -dz > 0.0 ? c[4].x : c[4].y;
  float4 coc = float4(r8.w, r7.w, r5.w, r4.w) * c[12].x + c[12].y;  // r6 (x r8, y r7, z r5, w r4)
  const float4 cut = float4(c255.z > r8.w, c255.z > r7.w, c255.z > r5.w, c255.z > r4.w);
  coc = cut > 0.0 ? c253.z : abs(coc);
  const float focus = exp2(c[3].z * log2(abs(max(saturate(abs(dz) * c[3].y), c254.z))));  // 52-57
  const float4 eq = float4(cC == coc.x, cC == coc.z, cC == coc.w, cC == coc.y);  // 58: r8, r5, r4, r7
  const float cnt = dot(eq, eq) + c254.w;
  float3 acc = eq.x * r8.xyz + r3.xyz;
  acc += eq.w * r7.xyz;
  acc += eq.y * r5.xyz;
  acc += eq.z * r4.xyz;
  const float w = saturate(c254.w - min(sel, focus));  // 62-63
  const float3 col = acc * (1.0 / cnt) * w;  // 66-67
  const float wsum = r1.w * c253.x + w;  // 68
  const float3 pre = (r1.xyz * c253.x + col) * (1.0 / wsum);  // 69

  float3 y, yh;
  UberTonemap(pre, y, yh);
  const float3 ramp = Ramp(t_prehud.Load(int3(id.xy, 0)).rgb, filt.w != 0);
  const float m = HudMask(t_guest.Load(int3(id.xy, 0)).rgb, ramp);
  o_resid[id.xy] = float4(Residual(SrgbToLinear(ramp), y, yh, c[9].w, scl.w, m), m);
}
