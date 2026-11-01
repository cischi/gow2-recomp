// Replacement for Gears of War 3's final post-process (guest pixel shader with
// ucode hash 2F284B68A112ED3E): the scene (tf0, 1280x720) blended with the
// 640x360 depth-of-field buffer (tf4: blurred RGB, alpha = sharpness), plus
// bloom (tf1, 322x182), a dither (tf2) and the color grading volume (tf3).
// tf4 stays at guest resolution (draw_resolution_scale_threshold): stretched
// bilinearly over the screen, an over-bright blurred background bleeds over
// sharp foreground edges (prison window bars) in 640x360 stair steps.
// At internal resolution > 1, on focus edges (the texels' sharpness differs)
// tf4 is upsampled with a joint bilateral filter: its texels weighted by how
// close the scene around each of them is to the scene at the pixel (log
// luminance), so the blend follows the scene's edges at the internal
// resolution. Elsewhere tf4 and the bloom are upsampled with a cubic B-spline,
// smooth where bilinear draws the texel grid on saturated blurred highlights
// (window bars out of focus). At scale 1 it is the guest program, identical
// to Xenia's translation.
// Declares the resources like Xenia's translation of the same shader (rtv
// path, bindless resources; the runtime checks the layout before replacing):
// the used float constants packed in order (c0 c3 c4 c5 c7 c8 c252 c253 c254
// c255) in b1; descriptor indices (sampler, unsigned SRV, signed SRV): tf1
// [0].y [0].z [0].w, tf2 [1].x [1].y [1].z, tf4 [1].w [2].x [2].y, tf0 [2].z
// [2].w [3].x, tf3 [3].y then 3D [3].z [3].w and stacked [4].x [4].y.
#define XE_DESCRIPTOR_INDEX_VECTORS 5
#define XE_TEXTURES_3D
#include "xenia_fetch.hlsli"
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[10]; }

// Xenia's tfetch3D of a 3D or stacked 2D texture (computed LOD).
float4 Fetch3D(Tex t, uint di_stacked_unsigned, uint di_stacked_signed, float3 uv) {
  bool is_3d = ((t.word5 >> 9) & 3u) == 2u;
  float3 size = is_3d ? float3(t.word2 & 0x7FFu, (t.word2 >> 11) & 0x7FFu, (t.word2 >> 22) & 0x3FFu)
                      : float3(t.word2 & 0x1FFFu, (t.word2 >> 13) & 0x1FFFu, (t.word2 >> 26) & 0x3Fu);
  size += 1.0;
  float3 p = uv + (1.5 / 1024.0) / size;
  if (!is_3d) p.z = min(max(uv.z * size.z + 1.5 / 1024.0, 0.5), size.z - 0.5);
  float2 g = GradientScale(t);
  float3 gx = ddx_coarse(p) * g.x, gy = ddy_coarse(p) * g.y;
  SamplerState smp = xe_samplers[t.sampler_index];
  uint4 code = SignCodes(t);
  bool4 used = UsedComponents(t);
  bool4 is_signed = code == 1u;
  bool all_signed = all(is_signed || !used), any_signed = any(is_signed && used);
  float4 v = 0.0, s = 0.0;
  if (is_3d) {
    if (!all_signed) v = xe_textures_3d[t.unsigned_index].SampleGrad(smp, p, gx, gy);
    if (any_signed) s = xe_textures_3d[t.signed_index].SampleGrad(smp, p, gx, gy);
  } else {
    uint su = xe_di[di_stacked_unsigned >> 2][di_stacked_unsigned & 3u];
    uint ss = xe_di[di_stacked_signed >> 2][di_stacked_signed & 3u];
    // Linear filtering between layers by the mag or min filter bit, as Xenia.
    bool minifying = max(gx.z, gy.z) * size.z > 1.0;
    float layer_t = 0.0;
    if ((minifying ? (t.word4 >> 1) : t.word4) & 1u) {
      p.z -= 0.5;
      layer_t = frac(p.z);
    }
    p.z = floor(p.z);
    if (!all_signed) v = xe_textures_2d[su].SampleGrad(smp, p, gx.xy, gy.xy);
    if (any_signed) s = xe_textures_2d[ss].SampleGrad(smp, p, gx.xy, gy.xy);
    if (layer_t != 0.0) {
      float4 v1 = 0.0, s1 = 0.0;
      float3 p1 = float3(p.xy, p.z + 1.0);
      if (!all_signed) v1 = xe_textures_2d[su].SampleGrad(smp, p1, gx.xy, gy.xy);
      if (any_signed) s1 = xe_textures_2d[ss].SampleGrad(smp, p1, gx.xy, gy.xy);
      v = lerp(v, v1, layer_t);
      s = lerp(s, s1, layer_t);
    }
  }
  return FetchResult(t, v, s);
}

// Ucode instructions 12..20: the scene's HDR encoding decoded (channels in
// x z y order), before the scale by c253.w.
float3 DecodeScene(float3 raw) {
  float4 c253 = xe_c[7], c254 = xe_c[8], c255 = xe_c[9];
  float3 r1 = float3(XMul(raw.x, c255.w), XMul(raw.y, c255.w), XMul(raw.z, c255.w));
  float3 r6 = floor(r1), r5 = frac(r1);
  float3 r4;
  r4.y = XMad(r5.y, c253.y, c253.y);
  r4.z = XMad(r5.z, c253.y, c253.y);
  r4.x = XMul(c253.y, exp2(r6.x));
  float3 r2 = float3(exp2(r6.z), exp2(r6.y), r5.x + c254.z);
  r4 = float3(XMul(r4.x, r2.z), XMul(r4.z, r2.x), XMul(r4.y, r2.y));
  return float3(CndEq(r6.x, r5.x, r4.x), CndEq(r6.z, r5.z, r4.y), CndEq(r6.y, r5.y, r4.z));
}

float LogLuminance(float3 c) { return log2(max(dot(max(c, 0.0), 1.0 / 3.0), 1.0 / 4096.0)); }

// Cubic B-spline filtered fetch from 4 bilinear ones (GPU Gems 2, chapter
// 20): a low resolution texture stretched over the screen without the
// stair-stepped contours of bilinear (bloom of an over-bright window).
float4 FetchBSpline(Tex t, float2 uv) {
  float2 size = GuestSize(t);
  float2 p = uv * size - 0.5, i = floor(p), f = p - i;
  float2 f2 = f * f, f3 = f2 * f;
  float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0, w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
  float2 w3 = f3 / 6.0, w2 = 1.0 - w0 - w1 - w3;
  float2 g0 = w0 + w1, g1 = w2 + w3;
  float2 h0 = (i - 0.5 + w1 / g0) / size, h1 = (i + 1.5 + w3 / g1) / size;
  return g0.y * (g0.x * Fetch(t, float2(h0.x, h0.y), 0.0, 0.0) + g1.x * Fetch(t, float2(h1.x, h0.y), 0.0, 0.0)) +
         g1.y * (g0.x * Fetch(t, float2(h0.x, h1.y), 0.0, 0.0) + g1.x * Fetch(t, float2(h1.x, h1.y), 0.0, 0.0));
}

// Host sample of the unsigned view at the top level, without Xenia's post
// processing (FetchResult): for the filter taps of float textures (tf4, the
// scene), where it is linear (exp_adjust) and applied once to the result.
float4 RawSample(Tex t, float2 uv) {
  return xe_textures_2d[t.unsigned_index].SampleLevel(xe_samplers[t.sampler_index], float3(uv, 0.0), 0.0);
}

// Log luminance of the scene under a tf4 texel (its 2x2 guest scene pixels;
// scene_uv: the texel's center in tf0 coordinates).
float TexelSceneLuminance(Tex t0, float2 scene_uv) {
  float2 scene_size = GuestSize(t0);
  float3 under = 0.0;
  [unroll] for (uint j = 0; j < 4; ++j)
    under += DecodeScene(FetchResult(t0, RawSample(t0, scene_uv + (float2(j & 1u, j >> 1) - 0.5) / scene_size), 0.0).xyz);
  return LogLuminance(under * 0.25);
}

// tf4 at uv (tf0 at scene_uv: the same screen point) upsampled as described
// above. Where the 2x2 bilinear texels have about the same sharpness (alpha:
// no focus edge) a cubic B-spline. Elsewhere the joint bilateral filter on 3x3
// texels, a tent of 1.5 texels times a range weight relative to the closest
// texel: a sharp pixel whose nearest texels are all mixed with the blurred
// background takes the farther ones like it instead of the bleeding blend.
float4 JointUpsample(Tex t4, Tex t0, float2 uv, float2 scene_uv) {
  float2 size = GuestSize(t4);
  float2 p = uv * size - 0.5, base = floor(p);
  float w_min = 1e30, w_max = -1e30;
  [unroll] for (uint i = 0; i < 4; ++i) {
    float w = RawSample(t4, (base + float2(i & 1u, i >> 1) + 0.5) / size).w;
    w_min = min(w_min, w);
    w_max = max(w_max, w);
  }
  float4 result;
  [branch] if (w_max - w_min < 1.0 / 16.0) {
    result = FetchBSpline(t4, uv);
  } else {
    float center = LogLuminance(DecodeScene(FetchResult(t0, RawSample(t0, scene_uv), 0.0).xyz));
    float2 nearest = floor(uv * size);
    float4 sum = 0.0;
    float weight_sum = 0.0;
    float d2[9];
    float d2_min = 1e30;
    [unroll] for (uint k = 0; k < 9; ++k) {
      float2 texel_uv = (nearest + float2(k % 3u, k / 3u) - 0.5) / size;
      float d = TexelSceneLuminance(t0, scene_uv + (texel_uv - uv)) - center;
      d2[k] = d * d;
      d2_min = min(d2_min, d2[k]);
    }
    [unroll] for (uint m = 0; m < 9; ++m) {
      float2 texel = nearest + float2(m % 3u, m / 3u) - 0.5;  // center, in texels
      float2 tent = saturate(1.0 - abs(texel - uv * size) / 1.5);
      float w = (tent.x * tent.y + 1.0 / 64.0) * exp2(d2_min - d2[m]);
      sum += w * RawSample(t4, texel / size);
      weight_sum += w;
    }
    result = FetchResult(t4, sum / weight_sum, 0.0);
  }
  return result;
}

float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD1, float4 v2 : TEXCOORD2, float4 pos : SV_Position,
            uint ff : SV_IsFrontFace) : SV_Target {
  float4 c0 = xe_c[0], c3 = xe_c[1], c4 = xe_c[2], c5 = xe_c[3], c7 = xe_c[4], c8 = xe_c[5];
  float4 c252 = xe_c[6], c253 = xe_c[7], c254 = xe_c[8], c255 = xe_c[9];
  Tex t1 = MakeTex(1u, 1u, 2u, 3u, 3u, true);     // bloom
  Tex t2 = MakeTex(2u, 4u, 5u, 6u, 1u, true);     // dither
  Tex t4 = MakeTex(4u, 7u, 8u, 9u, 4u, true);     // depth of field
  Tex t0 = MakeTex(0u, 10u, 11u, 12u, 3u, false);  // scene
  Tex t3 = MakeTex(3u, 13u, 14u, 15u, 3u, true);  // color grading
  // Internal resolution > 1: ratio between the scene's host and guest widths.
  uint scene_w, scene_h, scene_e;
  xe_textures_2d[t0.unsigned_index].GetDimensions(scene_w, scene_h, scene_e);
  bool scaled = round(float(scene_w) / GuestSize(t0).x) > 1.0;
  float4 r0 = v0, r1 = v1, r2 = v2, r3, r4 = 0, r5 = 0;
  // 5..7
  r0.xy = r2.xy + c3.xy;
  r2.x = max(r1.z, c8.x); r2.y = max(r1.w, c8.y); r2.z = XMul(c252.z, r0.x);
  r2.x = min(r2.x, c8.z); r2.y = min(r2.y, c8.w); r2.w = XMul(c252.z, r0.y);
  // 8..11
  float2 dx0 = ddx_coarse(r0.zw), dy0 = ddy_coarse(r0.zw);
  if (scaled)
    r0.xyz = FetchBSpline(t1, r0.zw).xyz;
  else
    r0.xyz = Fetch(t1, r0.zw, dx0, dy0).xyz;
  r5.w = Fetch(t2, r2.zw, ddx_coarse(r2.zw), ddy_coarse(r2.zw)).x;
  float2 dx2 = ddx_coarse(r2.xy), dy2 = ddy_coarse(r2.xy);
  if (scaled)
    r3 = JointUpsample(t4, t0, r2.xy, r1.xy);
  else
    r3 = Fetch(t4, r2.xy, dx2, dy2);
  r1.xyz = Fetch(t0, r1.xy, 0.0, 0.0).xyz;
  // 12..20 (r1.x = exp2(r6.x) and r2.xy are overwritten later: only the decoded r4 is kept)
  r4.xyz = DecodeScene(r1.xyz);
  // 17, 18 (the rest of them)
  r1.y = XMul(r3.x, c254.x); r1.z = XMul(r3.z, c254.x); r1.w = XMul(r3.y, c254.x);
  r2.w = r5.w + c254.w;
  // 21..22
  r4.x = XMad(r4.x, c253.w, -r1.y); r4.y = XMad(r4.y, c253.w, -r1.z); r4.z = XMad(r4.z, c253.w, -r1.w);
  {
    float y = XMad(r4.x, r3.w, r1.y), z = XMad(r4.z, r3.w, r1.w), w = XMad(r4.y, r3.w, r1.z);
    r1.y = y; r1.z = z; r1.w = w;
  }
  // 23..27
  r0.w = XMul(r1.w, c255.x) + XMul(r1.y, c255.y) + XMul(r1.z, c255.z);
  r0.x = XMul(r0.x, c254.x); r0.y = XMul(r0.y, c254.x); r0.z = XMul(r0.z, c254.x); r0.w = XMul(r0.w, c254.y);
  {
    float x = XMul(r0.x, c7.x), y = XMul(r0.z, c7.z), z = XMul(r0.y, c7.y);
    r0.x = x; r0.y = y; r0.z = z;
  }
  r1.x = saturate(XMul(c7.w, exp2(r0.w)));
  r3.x = XMad(r0.x, r1.x, r1.y); r3.y = XMad(r0.y, r1.x, r1.w); r3.z = XMad(r0.z, r1.x, r1.z);
  // 28..37
  r1.x = r3.x + c4.x; r1.y = r3.z + c4.x; r1.w = r3.y + c4.x;
  r0.x = 1.0 / abs(r1.x);
  r4.x = XMul(r3.x, c4.w); r4.y = XMul(r3.z, c4.w); r4.w = XMul(r3.y, c4.w);
  r0.y = 1.0 / abs(r1.y);
  r5.x = log2(abs(r4.x));
  r5.y = log2(abs(r4.y));
  r5.z = log2(abs(r4.w));
  r4.x = r3.x - c4.z; r4.y = r3.z - c4.z; r4.z = r3.y - c4.z;
  r0.z = 1.0 / abs(r1.w);
  r1.x = XMul(r5.x, c252.y); r1.y = XMul(r5.y, c252.y); r1.z = XMul(r5.z, c252.y);
  r4.x = saturate(XMul(r4.x, c253.z)); r4.y = saturate(XMul(r4.y, c253.z)); r4.z = saturate(XMul(r4.z, c253.z));
  r1.x = exp2(r1.x);
  r0.x = XMul(r3.x, r0.x); r0.y = XMul(r3.z, r0.y); r0.z = XMul(r3.y, r0.z);
  r1.y = exp2(r1.y);
  r0.x = XMul(r0.x, c4.y); r0.y = XMul(r0.y, c4.y); r0.z = XMul(r0.z, c4.y);
  r1.z = exp2(r1.z);
  // 38..46
  r3.xyz = r0.xyz - r1.xyz;
  r1.x = XMad(r4.x, r3.x, r1.x); r1.y = XMad(r4.y, r3.y, r1.y); r1.z = XMad(r4.z, r3.z, r1.z);
  r0.xyz = -r1.xyz + r0.xyz;
  r1.x = saturate(XMad(r0.x, c5.x, r1.x)); r1.y = saturate(XMad(r0.y, c5.x, r1.y));
  r1.z = saturate(XMad(r0.z, c5.x, r1.z));
  r0.xyz = -r1.xyz + c254.z;
  r3.x = XMul(r0.x, c3.w); r3.y = XMul(r0.y, c3.w); r3.z = XMul(r0.z, c3.w);
  r0.x = XMad(r3.x, r0.x, c253.x); r0.y = XMad(r3.y, r0.y, c253.x); r0.z = XMad(r3.z, r0.z, c253.x);
  r1.x = saturate(XMad(r2.w, r0.x, r1.x)); r1.y = saturate(XMad(r2.w, r0.y, r1.y));
  r1.z = saturate(XMad(r2.w, r0.z, r1.z));
  r1.x = XMad(r1.x, c252.x, c253.w); r1.y = XMad(r1.y, c252.x, c253.w); r1.z = XMad(r1.z, c252.x, c253.w);
  // 47..48
  r1.xyz = Fetch3D(t3, 16u, 17u, r1.xyz).xyz;
  return float4(XMul(r1.x, c0.x), XMul(r1.y, c0.x), XMul(r1.z, c0.x), 0.0) * asfloat(xe_sys[15].x);
}
