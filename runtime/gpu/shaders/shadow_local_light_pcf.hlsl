// Replacement for Gears of War 3's screen-space shadow projections of local
// lights: SPOT_LIGHT=1 for the spot light (guest pixel shader with ucode
// hash 857CECDEB64A657F), SPOT_LIGHT=0 for the point light
// (CA3F4A808B09D6F4). From the scene depth (tf0) they rebuild the position,
// project it into the shadow depth buffer (tf2) and write the shadowed
// light's modulation: lerp(1 + attenuation * (c7 + c254.x), 1, shadow^2),
// the attenuation being the distance falloff (times the cone falloff
// squared for the spot), the shadow optionally raised by a mask (tf1, bool
// b128). The game's 3x3 PCF is replaced by ShadowPcf (shadow_pcf.hlsli);
// everything else is the ucode's.
// Declares the resources like Xenia's translation of the same shaders (rtv
// path, bindless resources; the runtime checks the layout before replacing):
// xe_system_cbuffer b0, the used float constants packed in order in b1,
// bool constants b2, fetch constants b3, bindless descriptor indices b4
// (tf0: sampler [0].y, SRVs [0].z [0].w; tf2: [1].x, [1].y [1].z; tf1:
// [1].w, [2].x [2].y), sampler s0 space0, Texture2DArray t0 space1.
#define XE_DESCRIPTOR_INDEX_VECTORS 3
#include "xenia_fetch.hlsli"
#include "shadow_pcf.hlsli"
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 xe_bool[2]; }
#if SPOT_LIGHT
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[19]; }  // c1..c5 c7..c18 c254 c255
#define C_SLOPE xe_c[12]   // c14
#define C_MASK xe_c[11]    // c13
#define C_MATRIX 13        // c15..c18
#define C_254 xe_c[17]
#define C_255 xe_c[18]
#else
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[17]; }  // c1..c5 c7..c16 c254 c255
#define C_SLOPE xe_c[10]   // c12
#define C_MASK xe_c[4]     // c5
#define C_MATRIX 11        // c13..c16
#define C_254 xe_c[15]
#define C_255 xe_c[16]
#endif

[earlydepthstencil]
float4 main(float4 v0 : TEXCOORD0, float4 pos : SV_Position, uint ff : SV_IsFrontFace) : SV_Target {
  float4 c1 = xe_c[0], c2 = xe_c[1], c3 = xe_c[2], c4 = xe_c[3], c5 = xe_c[4], c7 = xe_c[5], c8 = xe_c[6];
  float4 c9 = xe_c[7], c10 = xe_c[8], c11 = xe_c[9], c254 = C_254, c255 = C_255;
  float4 m0 = xe_c[C_MATRIX], m1 = xe_c[C_MATRIX + 1], m2 = xe_c[C_MATRIX + 2], m3 = xe_c[C_MATRIX + 3];
  Tex t0 = MakeTex(0u, 1u, 2u, 3u, 1u, false);  // scene depth
  Tex t2 = MakeTex(2u, 4u, 5u, 6u, 1u, false);  // shadow depth
  Tex t1 = MakeTex(1u, 7u, 8u, 9u, 4u, true);   // mask
  float4 r0 = v0, r1 = 0, r2 = 0, r4 = 0, r6 = 0;
  // 7..12: scene depth at the pixel (r1.xy: screen coordinate).
  r1.z = 1.0 / r0.w;
  r1.xy = float2(XMul(r1.z, c1.x), XMul(r1.z, c1.y));
  r0 = float4(XMul(r1.z, r0.x), XMul(r1.y, r0.y), XMul(r1.z, r0.y), XMul(r1.x, r0.x));
  r1.xy = r0.wy + c1.wz;
  float2 dx = ddx_coarse(r1.xy), dy = ddy_coarse(r1.xy);
  r1.z = min(Fetch(t0, r1.xy, 0.0, 0.0).x, c255.z);
  // 13..20: view position, then the shadow map coordinate and the receiver's depth.
  r1.z = XMad(r1.z, c2.z, -c2.w);
  r0.y = 1.0 / r1.z;
  r0.xw = float2(XMul(r0.z, r0.y), XMul(r0.x, r0.y));
  r2 = float4(XMad(r0.y, m2.x, m3.x), XMad(r0.y, m2.y, m3.y), XMad(r0.y, m2.w, m3.w), XMad(r0.y, m2.z, m3.z));
  r2 = float4(XMad(r0.x, m1.x, r2.x), XMad(r0.x, m1.y, r2.y), XMad(r0.x, m1.w, r2.z), XMad(r0.x, m1.z, r2.w));
  r6 = float4(XMad(r0.w, m0.x, r2.x), XMad(r0.w, m0.y, r2.y), XMad(r0.w, m0.z, r2.w), XMad(r0.w, m0.w, r2.z));
  float2 uv = r6.xy * (1.0 / r6.w);
  // 32..42: world position, the light vector times c3.w (r4) and its squared length.
  r2.xyz = float3(XMad(r0.y, c10.x, c11.x), XMad(r0.y, c10.y, c11.y), XMad(r0.y, c10.z, c11.z));
  r0.xyz = float3(XMad(r0.x, c9.x, r2.x), XMad(r0.x, c9.z, r2.z), XMad(r0.x, c9.y, r2.y));
  r0.yzw = float3(XMad(r0.w, c8.x, r0.x), XMad(r0.w, c8.y, r0.z), XMad(r0.w, c8.z, r0.y));
  r2 = c7.wzyx + c254.x;
  float3 l = c3.xyz - r0.yzw;
  float ref = min(r6.z, c255.z);
  r4.xyz = float3(XMul(c3.w, l.z), XMul(c3.w, l.y), XMul(c3.w, l.x));
  float dd = XMul(r4.x, r4.x) + XMul(r4.z, r4.z) + XMul(r4.y, r4.y);
  // The filter (replaces the comparisons and their blend, see shadow_pcf.hlsli).
  float s = ShadowPcf(t2, uv, ref, C_SLOPE.z, c255.y);
  // Distance falloff saturate(1 - d^2)^c4.x, d = max(|r4| * c4.y + c4.z, 0).
  float d = max(XMad(sqrt(abs(dd)), c4.y, c4.z), c255.x);
  float atten = exp2(XMul(c4.x, log2(saturate(XMad(-d, d, c255.y)))));
#if SPOT_LIGHT
  // Cone falloff saturate((cos - c12.x) * c12.y), squared.
  float4 c12 = xe_c[10];
  r4.xyz *= rsqrt(abs(dd));
  float cos_angle = XMul(r4.x, -c5.z) + XMul(r4.z, -c5.x) + XMul(r4.y, -c5.y);
  float cone = saturate(XMul(cos_angle - c12.x, c12.y));
  atten = XMul(XMul(cone, atten), cone);
#endif
  r2 = float4(XMad(atten, r2.w, c255.y), XMad(atten, r2.z, c255.y), XMad(atten, r2.y, c255.y),
              XMad(atten, r2.x, c255.y));
  if (xe_bool[1].x & 1u) {  // b128
    float m = floor(XMad(Fetch(t1, r1.xy, dx, dy).w, C_MASK.x, c254.y));
    s = max(s, m);
  }
  s = XMul(s, s);
  float4 r = float4(XMad(s, c255.y - r2.x, r2.x), XMad(s, c255.y - r2.y, r2.y), XMad(s, c255.y - r2.z, r2.z),
                    XMad(s, c255.y - r2.w, r2.w));
  return r * asfloat(xe_sys[15].x);
}
