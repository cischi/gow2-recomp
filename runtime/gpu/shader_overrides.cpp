#include "gpu/shader_overrides.h"

#include <algorithm>
#include <format>

#include "log.h"
#include "shaders/dof_downsample_4x4.h"
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
      Add({0x0354AF5BA0F333D1ull, dof, kDofDownsample4x4Dxbc});
    }
  };
  static const Builtin registry;
  return registry;
}

}  // namespace gpu
