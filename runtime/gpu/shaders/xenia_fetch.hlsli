// Shared by the replacement shaders of guest pixel shaders: Xenia's resources
// (rtv path, bindless) and its translation of the Xenos ALU rules and texture
// fetches. Define XE_DESCRIPTOR_INDEX_VECTORS (uint4 count of
// xe_descriptor_indices_cbuffer, as in Xenia's translation) before including;
// the float constants cbuffer (b1) is declared by each shader.
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_sys[38]; }
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fc[48]; }
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 xe_di[XE_DESCRIPTOR_INDEX_VECTORS]; }
SamplerState xe_samplers[] : register(s0, space0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
#ifdef XE_TEXTURES_3D
Texture3D<float4> xe_textures_3d[] : register(t0, space2);
#endif

float XMul(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float XMad(float a, float b, float c) { return XMul(a, b) + c; }
float Sgt(float a, float b) { return a > b ? 1.0 : 0.0; }
float CndEq(float a, float b, float c) { return a == 0.0 ? b : c; }
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
  uint word2, word3, word4, word5;  // size, exp_adjust, lod bias, dimension
  uint signs;                       // 2 bits per component
  uint integer_scale_bits;          // xe_texture_integer_scale_bits
  uint sampler_index, unsigned_index, signed_index;
  uint components;
  bool computed_lod;
};

float2 GuestSize(Tex t) { return float2((t.word2 & 0x1FFFu) + 1u, ((t.word2 >> 13) & 0x1FFFu) + 1u); }

Tex MakeTex(uint fc, uint di_sampler, uint di_unsigned, uint di_signed, uint components, bool computed_lod) {
  Tex t;
  uint d = fc * 6u + 2u;  // dword 2 of the fetch constant
  t.word2 = xe_fc[d >> 2][d & 3u];
  t.word3 = xe_fc[(d + 1u) >> 2][(d + 1u) & 3u];
  t.word4 = xe_fc[(d + 2u) >> 2][(d + 2u) & 3u];
  t.word5 = xe_fc[(d + 3u) >> 2][(d + 3u) & 3u];
  t.signs = (xe_sys[11][fc >> 2] >> ((fc & 3u) * 8u)) & 0xFFu;
  t.integer_scale_bits = xe_sys[29 + (fc >> 2)][fc & 3u];
  t.sampler_index = xe_di[di_sampler >> 2][di_sampler & 3u];
  t.unsigned_index = xe_di[di_unsigned >> 2][di_unsigned & 3u];
  t.signed_index = xe_di[di_signed >> 2][di_signed & 3u];
  t.components = components;
  t.computed_lod = computed_lod;
  return t;
}

uint4 SignCodes(Tex t) { return uint4(t.signs & 3u, (t.signs >> 2) & 3u, (t.signs >> 4) & 3u, (t.signs >> 6) & 3u); }
bool4 UsedComponents(Tex t) { return bool4(true, t.components > 1, t.components > 2, t.components > 3); }

// Xenia's handling of a host sample: signedness (v unsigned, s signed view),
// bias, gamma, integer format emulation, exp_adjust.
float4 FetchResult(Tex t, float4 v, float4 s) {
  uint4 code = SignCodes(t);
  v = code == 1u ? s : v;
  uint bits = t.integer_scale_bits;
  [unroll] for (uint i = 0; i < 4; ++i) {
    if (code[i] == 2u) {
      // Biased: Xenia's 2 * v - 1 (with the exact unorm mapping when the width is known).
      uint w = (bits >> (6u * i)) & 0xFu;
      float m = float(1u << w);
      float exact = (v[i] * (m * 2.0 - 1.0) - m) / (m - 1.0);
      v[i] = w ? exact : v[i] * 2.0 - 1.0;
    } else if (code[i] == 3u) {
      v[i] = PwlGammaToLinear(v[i]);
    }
    if (bits & 0x03FFFFFFu) {
      uint field = (bits >> (6u * i)) & 0x3Fu;
      uint sign = (bits >> (6u * i + 4u)) & 3u;
      if (bits & 0x01000000u) {
        if (bits & 0x00FFFFFFu) {
          float scale = float(2u << (field & 0xFu));
          float q = round(v[i] * (scale - 1.0));
          q = (q * scale + q) / (scale * scale);
          if (field - 1u < 15u) v[i] = q;
        }
        float r = round(v[i] * 65536.0) * (1.0 / 65536.0);
        v[i] = sign ? v[i] : r;
        if (sign == 2u) v[i] = max(v[i], -1.0);
      } else {
        uint w = (field & 0xFu) + 1u;
        if (sign - 1u < 2u) w -= 1u;
        float m = float((1u << w) - 1u);
        v[i] = v[i] * max(m, 0.5) + (min(m, 0.5) - 0.5);
        if (bits & 0x04000000u) v[i] = round(v[i]);
      }
    }
  }
  return v * asfloat(uint(Ibfe(t.word3, 13, 6) * 0x00800000 + 0x3f800000));
}

// LOD scale (bias and upscaler bias) and per-axis anisotropy of a fetch with
// computed LOD: the gradient multipliers of Xenia's sample_d.
float2 GradientScale(Tex t) {
  float lod_scale = exp2(float(Ibfe(t.word4, 12, 10)) * (1.0 / 32.0) + asfloat(xe_sys[37].z));
  return lod_scale * float2(asfloat(uint(Ibfe(t.word4, 22, 5) * 0x00800000 + 0x3f800000)),
                            asfloat(uint(Ibfe(t.word4, 27, 5) * 0x00800000 + 0x3f800000)));
}

// Xenia's tfetch2D: host sampling at the coordinate (with the 1.5/1024
// texel epsilon), then FetchResult. dx/dy: screen derivatives of uv.
float4 Fetch(Tex t, float2 uv, float2 dx, float2 dy) {
  float3 p = float3(uv + (1.5 / 1024.0) / GuestSize(t), 0.0);
  SamplerState smp = xe_samplers[t.sampler_index];
  uint4 code = SignCodes(t);
  bool4 used = UsedComponents(t);
  bool4 is_signed = code == 1u;
  bool all_signed = all(is_signed || !used), any_signed = any(is_signed && used);
  float4 v = 0.0, s = 0.0;
  if (t.computed_lod) {
    float2 g = GradientScale(t);
    float2 gx = dx * g.x, gy = dy * g.y;
    if (!all_signed) v = xe_textures_2d[t.unsigned_index].SampleGrad(smp, p, gx, gy);
    if (any_signed) s = xe_textures_2d[t.signed_index].SampleGrad(smp, p, gx, gy);
  } else {
    float lod = float(Ibfe(t.word4, 12, 10)) * (1.0 / 32.0);
    if (!all_signed) v = xe_textures_2d[t.unsigned_index].SampleLevel(smp, p, lod);
    if (any_signed) s = xe_textures_2d[t.signed_index].SampleLevel(smp, p, lod);
  }
  return FetchResult(t, v, s);
}
