// Replacement for Gears of War 3's depth-of-field blur downsample (guest
// pixel shader with ucode hash 363BCAEF3EB3423B): from the 640x360 buffer of
// F820776A22228E1A (RGB colour, alpha the depth the blur is computed from) to
// 322x182, the input of the separable blur. The game averages the 4 taps and
// then weights the average by the blurriness of the average depth, B(a) =
// max(min(a >= c7.x ? c3.y : c3.x, saturate(|a - c7.x| * c7.y)^c7.z), c7.w),
// output (rgb * B, B) / 4 (premultiplied). Where an in-focus object (a face)
// meets an out-of-focus background, its colour enters the blur with the
// background's weight and spreads into the blurred background as a bright,
// blocky halo around the silhouette. Here every tap is weighted by its own
// blurriness before the sum: (sum rgb_i B(a_i) * c255.x, mean B(a_i)) * c255.x,
// identical to the game where the 4 depths are equal, while in-focus taps
// barely contribute.
// Declares the resources like Xenia's translation of the same shader (rtv
// path, bindless resources; the runtime checks the layout before replacing):
// xe_system_cbuffer b0, the used float constants packed in order (c3 c7
// c255) in b1, fetch constants b3, bindless descriptor indices b4 (tf0:
// sampler [0].y, SRVs [0].z [0].w), sampler s0 space0, Texture2DArray t0
// space1.
#define XE_DESCRIPTOR_INDEX_VECTORS 1
#include "xenia_fetch.hlsli"
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[3]; }  // c3 c7 c255

// Blurriness of the depth a (the game's instructions 15..22).
float Blurriness(float a, float4 c3, float4 c7) {
  float d = a - c7.x;
  float limit = d >= 0.0 ? c3.y : c3.x;
  float x = saturate(XMul(abs(d), c7.y));
  return max(min(limit, exp2(XMul(c7.z, log2(x)))), c7.w);
}

[earlydepthstencil]
float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD1, float4 pos : SV_Position,
            uint ff : SV_IsFrontFace) : SV_Target {
  float4 c3 = xe_c[0], c7 = xe_c[1], c255 = xe_c[2];
  Tex t0 = MakeTex(0u, 1u, 2u, 3u, 4u, true);
  float4 dx0 = ddx_coarse(v0), dy0 = ddy_coarse(v0), dx1 = ddx_coarse(v1), dy1 = ddy_coarse(v1);
  float4 taps[4] = {Fetch(t0, v1.wz, dx1.wz, dy1.wz), Fetch(t0, v1.xy, dx1.xy, dy1.xy),
                    Fetch(t0, v0.xy, dx0.xy, dy0.xy), Fetch(t0, v0.wz, dx0.wz, dy0.wz)};
  float3 rgb = 0.0;
  float weight = 0.0;
  [unroll] for (int i = 0; i < 4; ++i) {
    // The game's average depth is sum(|a| * c255.y) * c255.x: one tap's share times 4 is its own depth.
    float b = Blurriness(XMul(XMul(abs(taps[i].w), c255.y), 4.0 * c255.x), c3, c7);
    rgb += taps[i].rgb * b;
    weight += b;
  }
  // The game: (sum rgb * c255.x * B, B) * c255.x, B of the average depth.
  float4 r = float4(rgb * c255.x, weight * 0.25);
  return r * c255.x * asfloat(xe_sys[15].x);
}
