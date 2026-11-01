// Shared by the replacements of Gears of War 3's shadow projections (include
// after xenia_fetch.hlsli). The game filters the shadow depth with 9 point
// taps (3x3 texels at -0.5..1.5 in x, -1.5..0.5 in y) blended by
// getWeights2D: 4 bilinear comparisons one texel apart, a ~3 texel wide
// kernel centred at (+0.5, -0.5) texels that leaves the shadow map's texels
// as stair steps on magnified receivers. ShadowPcf filters the same soft
// comparison, saturate((depth - ref) * slope + one), with 4x4 bilinear
// comparisons one guest texel apart and binomial weights (1 3 3 1)/8 per
// axis (a ~5 texel wide kernel) with the same centre. Each bilinear
// comparison is a Gather of the host texels: at internal resolution > 1 the
// edges keep the resolution-scaled shadow map's detail while the softness,
// in guest texels, stays the same at every scale.

// Bilinear soft comparison at the guest texel position p (texel units, centres at .5) of the shadow depth.
float BilinearCompare(Tex t, float2 p, float2 host_per_guest, float2 host_size, float ref, float slope, float one,
                      float exp_scale) {
  float2 h = p * host_per_guest - 0.5;
  float2 b = floor(h), f = h - b;
  // Gather order: x (0,1), y (1,1), z (1,0), w (0,0) of the 2x2 texels from b.
  float4 d = xe_textures_2d[t.unsigned_index].GatherRed(xe_samplers[t.sampler_index], float3((b + 1.0) / host_size, 0.0));
  float4 c = saturate((d * exp_scale - ref) * slope + one);
  return lerp(lerp(c.w, c.z, f.x), lerp(c.x, c.y, f.x), f.y);
}

// The filtered comparison in [0, 1] (the game's dp4_sat of its 4 bilinear comparisons times 1/4) at the shadow map
// coordinate uv for the receiver depth ref.
float ShadowPcf(Tex t, float2 uv, float ref, float slope, float one) {
  float2 guest = GuestSize(t), host;
  {
    uint w, h, e;
    xe_textures_2d[t.unsigned_index].GetDimensions(w, h, e);
    host = float2(w, h);
  }
  // shortcut: only exp_adjust of FetchResult (the shadow depth is unsigned k_24_8, no gamma or integer scale).
  float exp_scale = asfloat(uint(Ibfe(t.word3, 13, 6) * 0x00800000 + 0x3f800000));
  float2 centre = uv * guest + float2(0.5, -0.5);
  static const float kWeights[4] = {1.0 / 8.0, 3.0 / 8.0, 3.0 / 8.0, 1.0 / 8.0};
  float s = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      s += kWeights[x] * kWeights[y] *
           BilinearCompare(t, centre + float2(x - 1.5, y - 1.5), host / guest, host, ref, slope, one, exp_scale);
    }
  }
  return saturate(s);
}
