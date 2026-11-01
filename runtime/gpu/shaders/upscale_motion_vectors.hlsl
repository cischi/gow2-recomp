// Motion vectors for the upscaler: the velocity that UE3's motion blur computes
// (GoW2: uber post, pixel shader ucode B6B6DD4E7D91F3D9, instructions 8-29), without its scale and its clamp for
// static pixels. Output in render pixels, from the current pixel to its position in the previous frame (FSR and
// DLSS convention).
//
// t0: scene depth (R32_FLOAT, render resolution); t1: velocity buffer (.xy, half resolution); the post constants
// in GoW2's uber post layout (UberPostConstants, gpu/upscale_passes.h).
#define RS "RootConstants(num32BitConstants=30, b0), DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0))"

cbuffer Constants : register(b0) {
  float4 c2, c13, c14, c15, c16, c17;
  uint2 size, velocity_size;
  uint camera_at_rest;  // CameraAtRest (upscale_passes.h): static pixels with no motion
  float velocity_step;  // quantization of an 8-bit velocity buffer: |v| within it is 0 (0.5 is not a code); else 0
};
Texture2D<float> depth_tex : register(t0);
Texture2D<float2> velocity_tex : register(t1);
RWTexture2D<float2> mv_out : register(u0);

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(size);
  float2 disp;  // UV displacement from the previous frame to this one
  const float2 velocity = velocity_tex.Load(int3(uint2(uv * float2(velocity_size)), 0));
  if (velocity.x > 0.0) {
    // 12-13: dynamic object, velocity normalized to the c17.zw limit (like the static branch after 26-29).
    float2 n = velocity * 2.0 - 1.0;
    n = abs(n) <= velocity_step ? 0.0 : n;
    disp = float2(n.x * c17.z * 0.5 / c17.x, n.y * c17.w * (-0.5 / c17.y));
  } else if (camera_at_rest) {
    disp = float2(0.0, 0.0);
  } else {
    // 11-12, 18-20: linear depth; below 14 units (very close, sky with z = 0) treated as infinity.
    float d = min(1.0 / (depth_tex.Load(int3(id.xy, 0)) * c2.z - c2.w), 65504.0);
    if (d < 14.0) d = 65504.0;
    // 16-25: position on the previous screen from (S * d, d, 1) and rows x, y, w of c13..c16; S in NDC, y up.
    const float2 s = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 prev = d * (s.x * c13.xyw + s.y * c14.xyw + c15.xyw) + c16.xyw;
    disp = abs(prev.z) > 1e-6 ? (s - prev.xy / prev.z) * float2(0.5, -0.5) : float2(0.0, 0.0);
  }
  mv_out[id.xy] = -disp * float2(size);
}
