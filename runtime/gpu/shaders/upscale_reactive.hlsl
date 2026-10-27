// Reactive mask for the upscaler: where the guest output (final image
// after the gamma ramp, with the HUD) differs from the ramp of the image captured right after the uber post (the same
// image without HUD), the upscaler must use the current frame and not the history. Continuous difference: a constant
// semi-transparent veil weighs little, the opaque HUD is 1.
//
// The ramp replicates Xenia's apply_gamma_table/apply_gamma_pwl shaders (IssueSwap): 256-entry table
// (DC_LUT_30_COLOR, red in bits 20:29) or 128-entry-per-channel PWL (DC_LUT_PWL_DATA, base in the low 16 bits).
#define RS "RootConstants(num32BitConstants=4, b0), DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0))"

cbuffer Constants : register(b0) {
  uint pwl;  // 0 table, 1 PWL
  uint3 padding;
};
Texture2D<float4> prehud_tex : register(t0);
Texture2D<float4> guest_tex : register(t1);
Buffer<uint> ramp_buf : register(t2);  // 256 table entries, then 384 PWL entries
RWTexture2D<float> reactive_out : register(u0);

#include "gamma_ramp.hlsli"

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 size;
  reactive_out.GetDimensions(size.x, size.y);
  if (any(id.xy >= size)) return;
  const float3 d = abs(guest_tex.Load(int3(id.xy, 0)).rgb - Ramp(prehud_tex.Load(int3(id.xy, 0)).rgb, pwl));
  // Below 4/1023 it is the same image (rounding); from 64/1023 up it is full HUD.
  reactive_out[id.xy] = smoothstep(4.0 / 1023.0, 64.0 / 1023.0, max(d.r, max(d.g, d.b)));
}
