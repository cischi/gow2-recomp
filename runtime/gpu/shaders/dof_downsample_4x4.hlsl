// Replacement for the UE3 depth-of-field 4x4 downsample (guest pixel shader
// with ucode hash 0354AF5BA0F333D1, Gears of War 2). For the 16 points of the
// 4x4 block: weight from depth (distance from the focal plane, clamped in
// front and behind), bonus for bright colors; color = sum of the weighted
// colors, alpha = sum of the weights, everything divided by 16.
// Declares the resources like Xenia's translation of the same shader
// (rtv path, bindless resources; the runtime checks the layout before
// replacing): xe_system_cbuffer b0, the used float constants packed in
// order (c2 c3 c4 c5 c6 c255) in b1, fetch constants b3, bindless descriptor
// indices b4 (tf1: sampler [0].y, unsigned SRV [0].z and signed [0].w;
// tf0: sampler [1].x, SRV [1].y and [1].z), sampler s0 space0,
// Texture2DArray t0 space1. Every read replicates Xenia's tfetch2D.
// At internal resolution s (host / guest texture size) it replicates the
// console's point read: finds the guest texel that contains the fetch
// coordinate and averages the centers of its s x s host texels.
// At scale 1 it is the console's single read, identical to Xenia.
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_sys[29]; }
cbuffer xe_float_cbuffer : register(b1) { float4 xe_c[6]; }  // c2 c3 c4 c5 c6 c255
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fc[48]; }
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 xe_di[2]; }
SamplerState xe_samplers[] : register(s0, space0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);

// Xenos multiplication: 0 * anything = 0.
float XMul(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float3 XMul3(float3 a, float b) { return float3(XMul(a.x, b), XMul(a.y, b), XMul(a.z, b)); }
float XMax(float a, float b) { return a >= b ? a : b; }
int Ibfe(uint v, uint offset, uint bits) { return int(v << (32u - offset - bits)) >> (32u - bits); }

float PwlGammaToLinear(float g) {
  float scale, offset;
  if (g >= 96.0 / 255.0) {
    bool hi = g >= 192.0 / 255.0;
    scale = hi ? 8.0 / 1024.0 : 4.0 / 1024.0;
    offset = hi ? -1024.0 : -256.0;
  } else {
    bool hi = g >= 64.0 / 255.0;
    scale = hi ? 2.0 / 1024.0 : 1.0 / 1024.0;
    offset = hi ? -64.0 : 0.0;
  }
  float l = saturate(g) * (255.0 * 1024.0);
  l = l * scale + offset;
  l += trunc(l * scale);
  return l * (1.0 / 1023.0);
}

struct Tex {
  uint word2, word3, word4;  // size, exp_adjust, lod bias
  uint signs;                // 2 bits per component
  uint sampler_index, unsigned_index, signed_index;
  uint components;
};

float2 GuestSize(Tex t) {
  return float2((t.word2 & 0x1FFFu) + 1u, ((t.word2 >> 13) & 0x1FFFu) + 1u);
}

// The console's tfetch2D with the offset in guest texels (like Xenia, including the 1.5/1024 epsilon).
// With scale > 1 it reads the center of host texel k (0..scale-1 per axis) inside the coordinate's guest texel.
float3 Fetch(Tex t, float2 uv_base, float2 dx, float2 dy, float2 offset_texels, uint2 k, uint2 scale) {
  float2 size = GuestSize(t);
  float2 p = uv_base + (offset_texels + 1.5 / 1024.0) / size;
  if (any(scale != 1u)) p = (floor(p * size) + (float2(k) + 0.5) / float2(scale)) / size;
  float3 uv = float3(p, 0.0);
  float lod_scale = exp2(float(Ibfe(t.word4, 12, 10)) * (1.0 / 32.0));
  float2 gx = dx * lod_scale, gy = dy * lod_scale;
  uint3 code = uint3(t.signs & 3u, (t.signs >> 2) & 3u, (t.signs >> 4) & 3u);
  bool3 is_signed = code == 1u;
  bool3 used = bool3(true, t.components > 1, t.components > 2);
  bool all_signed = all(is_signed || !used), any_signed = any(is_signed && used);
  float3 v = 0.0;
  if (!all_signed)
    v = xe_textures_2d[NonUniformResourceIndex(t.unsigned_index)].SampleGrad(
        xe_samplers[NonUniformResourceIndex(t.sampler_index)], uv, gx, gy).xyz;
  if (any_signed) {
    float3 s = xe_textures_2d[NonUniformResourceIndex(t.signed_index)].SampleGrad(
        xe_samplers[NonUniformResourceIndex(t.sampler_index)], uv, gx, gy).xyz;
    v = is_signed ? s : v;
  }
  [unroll] for (uint i = 0; i < 3; ++i) {
    if (code[i] == 2u) v[i] = v[i] * 2.0 - 1.0;
    else if (code[i] == 3u) v[i] = PwlGammaToLinear(v[i]);
  }
  return v * asfloat(uint(Ibfe(t.word3, 13, 6) * 0x00800000 + 0x3f800000));
}

float4 main(float4 v0 : TEXCOORD0, float4 pos : SV_Position, uint ff : SV_IsFrontFace) : SV_Target {
  Tex depth;  // tf0: fetch constant 0 (dword 0..5)
  depth.word2 = xe_fc[0].z; depth.word3 = xe_fc[0].w; depth.word4 = xe_fc[1].x;
  depth.signs = xe_sys[11].x & 0xFFu;
  depth.sampler_index = xe_di[1].x; depth.unsigned_index = xe_di[1].y; depth.signed_index = xe_di[1].z;
  depth.components = 1;
  Tex color;  // tf1: fetch constant 1 (dword 6..11)
  color.word2 = xe_fc[2].x; color.word3 = xe_fc[2].y; color.word4 = xe_fc[2].z;
  color.signs = (xe_sys[11].x >> 8) & 0xFFu;
  color.sampler_index = xe_di[0].y; color.unsigned_index = xe_di[0].z; color.signed_index = xe_di[0].w;
  color.components = 3;

  float4 c2 = xe_c[0], c3 = xe_c[1], c4 = xe_c[2], c5 = xe_c[3], c6 = xe_c[4], c255 = xe_c[5];

  // Internal resolution scale: ratio between the texture's host and guest sizes.
  uint2 s;
  {
    uint w, h, e;
    xe_textures_2d[NonUniformResourceIndex(color.unsigned_index)].GetDimensions(w, h, e);
    s = max(uint2(round(float2(w, h) / GuestSize(color))), 1u);
  }

  float2 dx = ddx_coarse(v0.xy), dy = ddy_coarse(v0.xy);
  float3 sum_color = 0.0;
  float sum_weight = 0.0;
  for (uint ty = 0; ty < 4; ++ty) {
    for (uint tx = 0; tx < 4; ++tx) {
      for (uint sy = 0; sy < s.y; ++sy) {
        for (uint sx = 0; sx < s.x; ++sx) {
          float d = Fetch(depth, v0.xy, dx, dy, float2(tx, ty), uint2(sx, sy), s).x;
          float3 col = XMul3(Fetch(color, v0.xy, dx, dy, float2(tx, ty), uint2(sx, sy), s), c6.x);
          float t = 1.0 / (XMul(d, c2.z) - c2.w) - c3.x;
          float near_far = (0.0 < -t) ? c4.x : c4.y;
          float p = XMax(abs(XMul(t, c3.y)), c255.z);
          float w = min(near_far, exp2(XMul(log2(abs(p)), c3.z)));
          float bright = any(col >= c255.x) ? 1.0 : 0.0;
          float w_color = XMul(bright, c5.x) + w;
          sum_color += XMul3(col, w_color);
          sum_weight += XMul(w, c255.x);
        }
      }
    }
  }
  float inv_count = 1.0 / float(s.x * s.y);
  float4 r = float4(sum_color * inv_count, sum_weight * inv_count);
  r = float4(XMul3(r.xyz, c255.y), XMul(r.w, c255.y));
  return r * asfloat(xe_sys[15].x);
}
