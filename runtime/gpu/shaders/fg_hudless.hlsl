// Frame generation: HUD-less image at output resolution (FSR 3's HUDLessColor). Where the HUD mask
// (upscale_reactive.hlsl, render resolution) is zero, the presented image is taken (base: upscaler output
// or guest output, sampled bilinearly as the presenter draws it); where there is HUD, the gamma ramp
// of the image captured before the HUD (prehud, bilinear). Mask dilated by one render texel.
// Also used by the upscaler: HUD-less scene as input (base = guest output, render resolution) and, with raw,
// HUD put back on top of the output (base = upscaler output, prehud = guest output without ramp).
#define RS "RootConstants(num32BitConstants=8, b0),DescriptorTable(SRV(t0, numDescriptors=4), UAV(u0)), " \
           "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, " \
           "addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

cbuffer Constants : register(b0) {
  uint pwl;  // 0 table, 1 PWL
  uint raw;  // 1: prehud_tex is already past the ramp (guest output), used as is
  uint2 padding;
  uint4 rect;  // left, top, right, bottom in output pixels: the image in the back buffer, excluding black bars
};
Texture2D<float4> base_tex : register(t0);
Texture2D<float4> prehud_tex : register(t1);
Texture2D<float> mask_tex : register(t2);
Buffer<uint> ramp_buf : register(t3);
RWTexture2D<float4> hudless_out : register(u0);
SamplerState lin : register(s0);

#include "gamma_ramp.hlsli"

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 size, rsize;
  hudless_out.GetDimensions(size.x, size.y);
  if (any(id.xy >= size)) return;
  if (any(id.xy < rect.xy) || any(id.xy >= rect.zw)) {
    hudless_out[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  mask_tex.GetDimensions(rsize.x, rsize.y);
  const float2 uv = (float2(id.xy - rect.xy) + 0.5) / float2(rect.zw - rect.xy);
  const float2 t = 1.0 / float2(rsize);
  float m = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) m = max(m, mask_tex.SampleLevel(lin, uv + float2(x, y) * t, 0));
  const float3 base = base_tex.SampleLevel(lin, uv, 0).rgb;
  const float3 over = prehud_tex.SampleLevel(lin, uv, 0).rgb;
  const float3 scene = raw ? over : Ramp(over, pwl);
  hudless_out[id.xy] = float4(lerp(base, scene, saturate(m * 4.0)), 1.0);
}
