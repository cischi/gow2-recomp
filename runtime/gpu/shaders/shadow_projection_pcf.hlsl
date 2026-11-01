// Replacement for Gears of War 3's screen-space shadow projection (guest pixel
// shader with ucode hash 3C5E875D17B49A36). From the scene depth (tf0) it
// rebuilds the position, projects it into the shadow depth buffer (tf1,
// 1120x688 or 560x344) and writes the light attenuation. The game's 3x3
// PCF is replaced by ShadowPcf (shadow_pcf.hlsli).
// Declares the resources like Xenia's translation of the same shader (rtv
// path, bindless resources; the runtime checks the layout before replacing):
// xe_system_cbuffer b0, the used float constants packed in order (c1 c2 c3
// c7 c8 c9 c10 c11 c255) in b1, fetch constants b3, bindless descriptor
// indices b4 (tf0: sampler [0].y, unsigned SRV [0].z and signed [0].w; tf1:
// sampler [1].x, SRV [1].y and [1].z), sampler s0 space0, Texture2DArray t0
// space1. The ALU code before the filter is the ucode's (instructions 5..18).
#define XE_DESCRIPTOR_INDEX_VECTORS 2
#include "xenia_fetch.hlsli"
#include "shadow_pcf.hlsli"
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[9]; }  // c1 c2 c3 c7 c8 c9 c10 c11 c255

[earlydepthstencil]
float4 main(float4 v0 : TEXCOORD0, float4 pos : SV_Position, uint ff : SV_IsFrontFace) : SV_Target {
  float4 c1 = xe_c[0], c2 = xe_c[1], c3 = xe_c[2], c7 = xe_c[3], c8 = xe_c[4], c9 = xe_c[5], c10 = xe_c[6];
  float4 c11 = xe_c[7], c255 = xe_c[8];
  Tex t0 = MakeTex(0u, 1u, 2u, 3u, 1u, false);  // scene depth
  Tex t1 = MakeTex(1u, 4u, 5u, 6u, 1u, false);  // shadow depth
  float4 r0 = v0, r1 = 0, r2 = 0;
  // 5..10: scene depth at the pixel.
  r1.x = 1.0 / r0.w;
  r1.y = XMul(r1.x, c1.x); r1.z = XMul(r1.x, c1.y);
  r0 = float4(XMul(r1.y, r0.x), XMul(r1.x, r0.x), XMul(r1.x, r0.y), XMul(r1.z, r0.y));
  r1.xy = r0.xw + c1.wz;
  r1.x = Fetch(t0, r1.xy, 0.0, 0.0).x;
  r1.x = min(r1.x, c255.z);
  // 11..18: view position, then the shadow map coordinate (r2.zw) and the receiver's depth (r2.y).
  r1.x = XMad(r1.x, c2.z, -c2.w);
  r0.x = 1.0 / r1.x;
  r1.xy = float2(XMul(r0.y, r0.x), XMul(r0.z, r0.x));
  r0 = float4(XMad(r0.x, c10.x, c11.x), XMad(r0.x, c10.y, c11.y), XMad(r0.x, c10.w, c11.w), XMad(r0.x, c10.z, c11.z));
  r0 = float4(XMad(r1.y, c9.x, r0.x), XMad(r1.y, c9.y, r0.y), XMad(r1.y, c9.w, r0.z), XMad(r1.y, c9.z, r0.w));
  r0 = float4(XMad(r1.x, c8.z, r0.w), XMad(r1.x, c8.x, r0.x), XMad(r1.x, c8.y, r0.y), XMad(r1.x, c8.w, r0.z));
  r2.y = min(r0.x, c255.z);
  r0.x = 1.0 / r0.w;
  r2.zw = float2(XMul(r0.y, r0.x), XMul(r0.z, r0.x));
  // The filter (replaces 19..44, see shadow_pcf.hlsli).
  float s = ShadowPcf(t1, r2.zw, r2.y, c7.z, c255.y);
  // 45..47: the game's shaping of the attenuation.
  r0.x = XMad(s, s, -c255.y);
  r0.x = XMad(r0.x, c3.x, c255.y);
  return sqrt(abs(r0.x)) * asfloat(xe_sys[15].x);
}
