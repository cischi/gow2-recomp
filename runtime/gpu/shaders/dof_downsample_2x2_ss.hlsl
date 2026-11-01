// Replacement for Gears of War 3's depth-of-field 2x2 downsample (guest
// pixel shader with ucode hash F820776A22228E1A). From a 1280x720 scene to
// 640x360: RGB = average of 4 decoded scene taps, alpha = a mask from 4 taps
// of a 640x360 map times the depth of the 2x2 block farthest from its mean.
// That alpha drives the final sharp/blurred blend, and being a per-pixel pick
// at half resolution it outlines bright backgrounds behind thin objects
// (prison bars) with 2x2 stair steps.
// At internal resolution s (host / guest scene size) the guest program runs
// on a grid of min(s, 2) x min(s, 2) points of each guest texel (all taps
// shifted together by the same sub-texel offset) and the results are
// averaged: the edges get a partial coverage (3x3 at 3x costs ~0.6 ms more
// for no visible gain). At scale 1 it is the guest program once, identical
// to Xenia's translation.
// Declares the resources like Xenia's translation of the same shader (rtv
// path, bindless resources; the runtime checks the layout before replacing):
// xe_system_cbuffer b0, the used float constants packed in order (c2 c253
// c254 c255) in b1, fetch constants b3, bindless descriptor indices b4
// (tf2: sampler [0].y, unsigned SRV [0].z and signed [0].w; tf0: sampler
// [1].x, SRV [1].y and [1].z; tf1: sampler [1].w, SRV [2].x and [2].y),
// sampler s0 space0, Texture2DArray t0 space1. Every read replicates Xenia's
// tfetch2D, and the ALU code the ucode (Xenos 0 * anything = 0). The
// descriptor indices are uniform across the draw: no NonUniformResourceIndex.
#define XE_DESCRIPTOR_INDEX_VECTORS 3
#include "xenia_fetch.hlsli"
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[4]; }  // c2 c253 c254 c255

// The guest program (ucode instructions 6..70) with the four tap coordinates
// in r0/r1 (interpolators v0/v1) and their screen derivatives.
float4 Guest(float4 r0, float4 r1, float4 dx0, float4 dy0, float4 dx1, float4 dy1, Tex t0, Tex t1, Tex t2) {
  float4 c2 = xe_c[0], c253 = xe_c[1], c254 = xe_c[2], c255 = xe_c[3];
  float4 r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0;
  float4 r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0, r16 = 0;
  float4 t;
  r8.xy = Fetch(t2, r1.wz, dx1.wz, dy1.wz).xy;
  r7.yz = Fetch(t2, r1.xy, dx1.xy, dy1.xy).xy;
  r6.x = Fetch(t0, r0.xy, dx0.xy, dy0.xy).x;
  r6.y = Fetch(t0, r0.wz, dx0.wz, dy0.wz).x;
  r6.z = Fetch(t0, r1.xy, dx1.xy, dy1.xy).x;
  r6.w = Fetch(t0, r1.wz, dx1.wz, dy1.wz).x;
  r9.xy = Fetch(t2, r0.xy, dx0.xy, dy0.xy).xy;
  r9.zw = Fetch(t2, r0.wz, dx0.wz, dy0.wz).xy;
  r3.xyz = Fetch(t1, r0.xy, dx0.xy, dy0.xy).xyz;
  r2.xyz = Fetch(t1, r0.wz, dx0.wz, dy0.wz).xyz;
  r4.xyz = Fetch(t1, r1.wz, dx1.wz, dy1.wz).xyz;
  t = Fetch(t1, r1.xy, dx1.xy, dy1.xy);
  r0.xyw = t.xyz;
  // 18..20
  r5.x = XMul(r4.x, c254.z); r5.y = XMul(r4.y, c254.z); r5.z = XMul(r4.z, c254.z);
  r1.y = XMul(c254.z, r0.x);
  r2.x = XMul(r2.x, c254.z); r2.y = XMul(r2.y, c254.z); r2.z = XMul(r2.z, c254.z);
  r1.z = XMul(c254.z, r0.y);
  r3.x = XMul(r3.x, c254.z); r3.y = XMul(r3.y, c254.z); r3.z = XMul(r3.z, c254.z);
  r1.w = XMul(c254.z, r0.w);
  // 21..25
  r4.xyz = floor(r3.xzy); r7.x = r9.x + r9.y;
  r3.xyz = frac(r3.xyz); r7.w = r9.z + r9.w;
  r9.xyz = floor(r1.ywz); r11.x = floor(r2.x);
  r13.xyz = floor(r5.xzy); r11.y = floor(r2.z);
  r12.xyz = frac(r5.xyz); r11.z = floor(r2.y);
  // 26..35
  r14.x = XMad(r12.x, c255.x, c255.x); r14.y = XMad(r12.y, c255.x, c255.x);
  r10.xyz = frac(r2.xyz); r1.x = exp2(r13.x);
  r15.x = XMad(r10.x, c255.x, c255.x); r15.y = XMad(r10.y, c255.x, c255.x);
  r2.xyz = frac(r1.yzw); r5.x = exp2(r11.x);
  r16.y = XMad(r2.z, c255.x, c255.x); r16.z = XMad(r2.y, c255.x, c255.x);
  r6 = min(r6, c253.x); r0.x = exp2(r9.y);
  r5.z = r3.x + c254.w; r0.z = exp2(r9.z);
  r1.z = r12.z + c254.w; r1.y = exp2(r9.x);
  r5.w = r10.z + c254.w; r1.w = exp2(r11.y);
  r0.w = r2.x + c254.w; r2.w = exp2(r13.y);
  // 36..42
  r6.x = XMad(r6.x, c2.z, -c2.w); r6.y = XMad(r6.y, c2.z, -c2.w);
  r6.z = XMad(r6.z, c2.z, -c2.w); r6.w = XMad(r6.w, c2.z, -c2.w);
  r7.x = Sgt(r7.x, c253.y); r7.w = Sgt(r7.w, c253.y); r0.y = exp2(r4.x);
  r14.z = XMul(r2.w, c255.x); r6.x = 1.0 / r6.x;
  r15.z = XMul(r1.w, c255.x); r6.y = 1.0 / r6.y;
  r16.x = XMul(r1.y, c255.x); r6.z = 1.0 / r6.z;
  {
    float a = XMul(r16.x, r0.w), b = XMul(r16.y, r0.x), c = XMul(r16.z, r0.z);
    r0.x = a; r0.z = b; r0.w = c;
  }
  r6.w = 1.0 / r6.w;
  r8.z = XMul(r6.w, c255.z) + XMul(r6.z, c255.z) + XMul(r6.x, c255.z) + XMul(r6.y, c255.z);
  r5.y = exp2(r11.z);
  // 43..48
  r5.x = XMul(r15.x, r5.x); r5.y = XMul(r15.y, r5.y); r5.w = XMul(r15.z, r5.w);
  r1.y = exp2(r13.z);
  r1.x = XMul(r14.x, r1.x); r1.y = XMul(r14.y, r1.y); r1.z = XMul(r14.z, r1.z);
  {
    float x = CndEq(r13.x, r12.x, r1.x), y = CndEq(r13.y, r12.z, r1.z), z = CndEq(r13.z, r12.y, r1.y);
    r1.x = x; r1.y = y; r1.z = z;
  }
  {
    float x = CndEq(r11.x, r10.x, r5.x), y = CndEq(r11.y, r10.z, r5.w), w = CndEq(r11.z, r10.y, r5.y);
    r5.x = x; r5.y = y; r5.w = w;
  }
  r6 = r6.wyxz - r8.z;
  {
    float x = CndEq(r9.x, r2.x, r0.x), z = CndEq(r9.y, r2.z, r0.z), w = CndEq(r9.z, r2.y, r0.w);
    r0.x = x; r0.z = z; r0.w = w;
  }
  // 49..62
  r2.x = XMul(r0.x, c254.x); r2.y = XMul(r0.w, c254.x); r2.z = XMul(r0.z, c254.x);
  r0.x = Sgt(abs(r6.z), abs(r6.y));
  {
    float x = XMad(r5.x, c254.x, r2.x), y = XMad(r5.w, c254.x, r2.y), z = XMad(r5.y, c254.x, r2.z);
    r2.x = x; r2.y = y; r2.z = z;
  }
  r0.x = CndEq(r0.x, r6.z, r6.y);
  r0.z = Sgt(abs(r0.x), abs(r6.w));
  r0.x = CndEq(r0.z, r0.x, r6.w);
  r0.z = Sgt(abs(r0.x), abs(r6.x)); r6.z = r7.x + r7.w;
  r8.w = CndEq(r0.z, r0.x, r6.x);
  r6.x = r8.x + r8.y; r6.y = r8.z + r8.w; r6.w = r7.y + r7.z;
  r0.x = Sgt(r6.x, c253.y); r0.z = Sgt(r6.w, c253.y);
  r0.w = r6.z + r0.z;
  r0.x = r0.w + r0.x;
  r0.x = Sgt(r0.x, c254.w);
  r3.w = r0.x > 0.0 ? c254.w : c254.y;
  // 63..70
  {
    float x = XMul(r3.w, c255.y), y = XMul(r3.y, c255.x), w = XMul(r3.z, c255.x);
    r0.x = XMul(c255.x, r0.y);
    r5.x = x; r5.y = y; r5.w = w;
  }
  r0.w = XMul(r5.x, r6.y); r5.x = exp2(r4.y);
  r0.y = r5.w + c255.x; r0.z = r5.y + c255.x; r5.y = exp2(r4.z);
  r0.x = XMul(r0.x, r5.z); r0.y = XMul(r0.y, r5.x); r0.z = XMul(r0.z, r5.y);
  {
    float x = CndEq(r4.x, r3.x, r0.x), y = CndEq(r4.y, r3.z, r0.y), z = CndEq(r4.z, r3.y, r0.z);
    r0.x = x; r0.y = y; r0.z = z;
  }
  {
    float x = XMad(r0.x, c254.x, r2.x), y = XMad(r0.y, c254.x, r2.z), z = XMad(r0.z, c254.x, r2.y);
    r0.x = x; r0.y = y; r0.z = z;
  }
  {
    float x = XMad(r1.x, c254.x, r0.x), y = XMad(r1.z, c254.x, r0.z), z = XMad(r1.y, c254.x, r0.y);
    r0.x = x; r0.y = y; r0.z = z;
  }
  return float4(XMul(r0.x, c255.z), XMul(r0.y, c255.z), XMul(r0.z, c255.z), XMul(r0.w, c255.w));
}

float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD1, float4 pos : SV_Position,
            uint ff : SV_IsFrontFace) : SV_Target {
  Tex t0 = MakeTex(0u, 4u, 5u, 6u, 1u, false);  // depth
  Tex t1 = MakeTex(1u, 7u, 8u, 9u, 3u, true);   // scene color
  Tex t2 = MakeTex(2u, 1u, 2u, 3u, 2u, true);   // 640x360 mask
  float4 dx0 = ddx_coarse(v0), dy0 = ddy_coarse(v0), dx1 = ddx_coarse(v1), dy1 = ddy_coarse(v1);
  // Internal resolution scale: ratio between the scene's host and guest sizes, at most 2 (see above).
  uint2 s;
  {
    uint w, h, e;
    xe_textures_2d[t1.unsigned_index].GetDimensions(w, h, e);
    s = clamp(uint2(round(float2(w, h) / GuestSize(t1))), 1u, 2u);
  }
  float4 sum = 0.0;
  for (uint sy = 0; sy < s.y; ++sy) {
    for (uint sx = 0; sx < s.x; ++sx) {
      float2 o = ((float2(sx, sy) + 0.5) / float2(s) - 0.5) / GuestSize(t1);
      sum += Guest(v0 + o.xyyx, v1 + o.xyyx, dx0, dy0, dx1, dy1, t0, t1, t2);
    }
  }
  return sum / float(s.x * s.y) * asfloat(xe_sys[15].x);
}
