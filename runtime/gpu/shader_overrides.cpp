#include "gpu/shader_overrides.h"

#include <algorithm>
#include <format>

#include "game_profile.h"
#include "log.h"
#include "shaders/dof_blur_downsample_weighted.h"
#include "shaders/dof_composite_joint.h"
#include "shaders/dof_downsample_2x2_ss.h"
#include "shaders/dof_downsample_4x4.h"
#include "shaders/shadow_point_light_pcf.h"
#include "shaders/shadow_projection_pcf.h"
#include "shaders/shadow_spot_light_pcf.h"
#include "xenia/gpu/dxbc_shader_translator.h"

namespace gpu {

std::optional<std::string> LayoutMismatch(const ShaderLayout& e, const ShaderLayout& a) {
  if (e.rov != a.rov) return "ROV path";
  if (e.bindless != a.bindless) return "non-bindless resources";
  if (e.interpolator_mask != a.interpolator_mask || e.interpolators_centroid != a.interpolators_centroid ||
      e.param_gen != a.param_gen)
    return "interpolators";
  if (e.textures != a.textures) return "texture";
  if (e.samplers != a.samplers) return "sampler";
  if (e.float_constants != a.float_constants) return "float constants";
  if (e.bool_constants_used != a.bool_constants_used || e.loop_constants_used != a.loop_constants_used)
    return "bool/loop constants";
  return std::nullopt;
}

bool ShaderOverrides::FirstReport(uint64_t ucode_hash, uint64_t modification) const {
  std::lock_guard lock(mutex_);
  return reported_.insert({ucode_hash, modification}).second;
}

ShaderLayout LayoutOf(const xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov, bool bindless) {
  const auto& shader = static_cast<const xe::gpu::DxbcShader&>(t.shader());
  const xe::gpu::DxbcShaderTranslator::Modification mod(t.modification());
  ShaderLayout l{};
  l.rov = rov;
  l.bindless = bindless;
  l.interpolator_mask = mod.pixel.interpolator_mask;
  l.interpolators_centroid = mod.pixel.interpolators_centroid;
  l.param_gen = mod.pixel.param_gen_enable;
  for (const auto& b : shader.GetTextureBindingsAfterTranslation())
    l.textures.push_back({b.bindless_descriptor_index, b.fetch_constant, uint32_t(b.dimension), b.is_signed});
  for (const auto& b : shader.GetSamplerBindingsAfterTranslation())
    l.samplers.push_back({b.bindless_descriptor_index, b.fetch_constant, uint32_t(b.mag_filter),
                          uint32_t(b.min_filter), uint32_t(b.mip_filter), uint32_t(b.aniso_filter)});
  const auto& map = shader.constant_register_map();
  std::copy(std::begin(map.float_bitmap), std::end(map.float_bitmap), l.float_constants.begin());
  for (uint32_t word : map.bool_bitmap) l.bool_constants_used |= word;
  l.loop_constants_used = map.loop_bitmap;
  return l;
}

void ApplyOverride(const ShaderOverrides& registry, xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov,
                   bool bindless) {
  const uint64_t hash = t.shader().ucode_data_hash();
  const ShaderOverride* o = registry.Find(hash);
  if (!o) return;
  const auto why = LayoutMismatch(o->layout, LayoutOf(t, rov, bindless));
  if (!why) t.ReplaceTranslatedBinary({o->dxbc.begin(), o->dxbc.end()});
  if (!registry.FirstReport(hash, t.modification())) return;
  if (why)
    LogLine(std::format("[gpu] replacement shader {:016X}: layout differs from the expected one ({}), using Xenia's "
                        "translation",
                        hash, *why));
  else
    LogLine(std::format("[gpu] replacement shader {:016X} active", hash));
}

const ShaderOverrides& BuiltinShaderOverrides() {
  struct Builtin : ShaderOverrides {
    Builtin() {
      // 4x4 depth-of-field downsample: layout of Xenia's translation of the
      // synthetic shader in tests/gpu/d3d12_shader_override_test.cpp
      // (same fetches, constants and interpolators as the game's shader).
      // Textures and samplers: tf1 (color) then tf0 (depth), 2D, filters
      // from the fetch constant (3 = kUseFetchConst, 7 = AnisoFilter::kUseFetchConst).
      // Float constants c2..c6 and c255.
      ShaderLayout dof{};
      dof.bindless = true;
      dof.interpolator_mask = 1;
      dof.textures = {{2, 1, 1, false}, {3, 1, 1, true}, {5, 0, 1, false}, {6, 0, 1, true}};
      dof.samplers = {{1, 1, 3, 3, 3, 7}, {4, 0, 3, 3, 3, 7}};
      dof.float_constants = {0x7C, 0, 0, 1ull << 63};
      // The game's shader with this layout: game_profile::Profile::dof_downsample_hash (0 = none).
      if (const uint64_t h = game_profile::Get().dof_downsample_hash) Add({h, dof, kDofDownsample4x4Dxbc});
      // 2x2 depth-of-field downsample supersampled at the internal resolution: layout of Xenia's translation of
      // the synthetic shader in tests/gpu/d3d12_shader_override_test.cpp (same fetches as Gears of War 3's).
      // Textures and samplers: tf2 (mask), tf0 (depth, no computed LOD: anisotropy off), tf1 (color). Float
      // constants c2, c253..c255.
      ShaderLayout dof2{};
      dof2.bindless = true;
      dof2.interpolator_mask = 0b11;
      dof2.textures = {{2, 2, 1, false}, {3, 2, 1, true}, {5, 0, 1, false},
                       {6, 0, 1, true},  {8, 1, 1, false}, {9, 1, 1, true}};
      dof2.samplers = {{1, 2, 3, 3, 3, 7}, {4, 0, 3, 3, 3, 0}, {7, 1, 3, 3, 3, 7}};
      dof2.float_constants = {0x4, 0, 0, 0x7ull << 61};
      if (const uint64_t h = game_profile::Get().dof_downsample_2x2_hash) Add({h, dof2, kDofDownsample2x2SsDxbc});
      // Final post-process with the depth-of-field buffer upsampled along the scene's edges (Gears of War 3's):
      // layout of Xenia's translation of the game's shader. Textures and samplers: tf1 (bloom), tf2 (dither),
      // tf4 (depth of field), tf0 (scene, no computed LOD: anisotropy off), tf3 (color grading: 3D, then stacked
      // 2D). Float constants c0 c3 c4 c5 c7 c8 c252..c255.
      ShaderLayout comp{};
      comp.bindless = true;
      comp.interpolator_mask = 0b111;
      comp.textures = {{2, 1, 1, false},  {3, 1, 1, true},  {5, 2, 1, false},  {6, 2, 1, true},
                       {8, 4, 1, false},  {9, 4, 1, true},  {11, 0, 1, false}, {12, 0, 1, true},
                       {14, 3, 2, false}, {15, 3, 2, true}, {16, 3, 1, false}, {17, 3, 1, true}};
      comp.samplers = {{1, 1, 3, 3, 3, 7}, {4, 2, 3, 3, 3, 7}, {7, 4, 3, 3, 3, 7}, {10, 0, 3, 3, 3, 0},
                       {13, 3, 3, 3, 3, 7}};
      comp.float_constants = {0x1B9, 0, 0, 0xFull << 60};
      if (const uint64_t h = game_profile::Get().dof_composite_hash) Add({h, comp, kDofCompositeJointDxbc});
      // Depth-of-field blur downsample weighting each tap by its blurriness (Gears of War 3's): layout of Xenia's
      // translation of the game's shader. Texture and sampler tf0 (2D, computed LOD). Float constants c3 c7 c255.
      ShaderLayout blur{};
      blur.bindless = true;
      blur.interpolator_mask = 0b11;
      blur.textures = {{2, 0, 1, false}, {3, 0, 1, true}};
      blur.samplers = {{1, 0, 3, 3, 3, 7}};
      blur.float_constants = {0x88, 0, 0, 1ull << 63};
      if (const uint64_t h = game_profile::Get().dof_blur_downsample_hash)
        Add({h, blur, kDofBlurDownsampleWeightedDxbc});
      // Screen-space shadow projection with a wider PCF (Gears of War 3's): layout of Xenia's translation of the
      // game's shader. Textures and samplers: tf0 (scene depth), tf1 (shadow depth), 2D, no computed LOD. Float
      // constants c1 c2 c3 c7..c11 c255.
      ShaderLayout shadow{};
      shadow.bindless = true;
      shadow.interpolator_mask = 1;
      shadow.textures = {{2, 0, 1, false}, {3, 0, 1, true}, {5, 1, 1, false}, {6, 1, 1, true}};
      shadow.samplers = {{1, 0, 3, 3, 3, 0}, {4, 1, 3, 3, 3, 0}};
      shadow.float_constants = {0xF8E, 0, 0, 1ull << 63};
      if (const uint64_t h = game_profile::Get().shadow_projection_hash) Add({h, shadow, kShadowProjectionPcfDxbc});
      // Shadow projections of spot and point lights with the same PCF: textures and samplers tf0 (scene depth), tf2
      // (shadow depth), both without computed LOD, then tf1 (mask). Float constants c1..c5, c7..c18 (spot) or
      // c7..c16 (point), c254 c255; bool constant b128.
      ShaderLayout local{};
      local.bindless = true;
      local.interpolator_mask = 1;
      local.textures = {{2, 0, 1, false}, {3, 0, 1, true}, {5, 2, 1, false},
                        {6, 2, 1, true},  {8, 1, 1, false}, {9, 1, 1, true}};
      local.samplers = {{1, 0, 3, 3, 3, 0}, {4, 2, 3, 3, 3, 0}, {7, 1, 3, 3, 3, 7}};
      local.bool_constants_used = 1;
      local.float_constants = {0x7FFBE, 0, 0, 0x3ull << 62};
      if (const uint64_t h = game_profile::Get().shadow_spot_light_hash) Add({h, local, kShadowSpotLightPcfDxbc});
      local.float_constants[0] = 0x1FFBE;
      if (const uint64_t h = game_profile::Get().shadow_point_light_hash) Add({h, local, kShadowPointLightPcfDxbc});
    }
  };
  static const Builtin registry;
  return registry;
}

}  // namespace gpu
