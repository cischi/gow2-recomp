// Registry of replacement shaders by ucode hash, and layout check:
// the replacement DXBC is valid only if Xenia's translation exposes the same
// resources the DXBC expects. Registry and comparison use only std types;
// LayoutOf/ApplyOverride read Xenia's translation (hook of vendor patch
// 0010, SetTranslatedPixelShaderHook).
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/gpu/d3d12/d3d12_shader.h"

namespace gpu {

struct TextureSlot {
  uint32_t descriptor_index;
  uint32_t fetch_constant;
  uint32_t dimension;  // xenos::FetchOpDimension
  bool is_signed;
  bool operator==(const TextureSlot&) const = default;
};

struct SamplerSlot {
  uint32_t descriptor_index;
  uint32_t fetch_constant;
  uint32_t mag_filter, min_filter, mip_filter, aniso_filter;
  bool operator==(const SamplerSlot&) const = default;
};

struct ShaderLayout {
  bool rov;
  bool bindless;
  uint32_t interpolator_mask;
  uint32_t interpolators_centroid;
  bool param_gen;
  std::vector<TextureSlot> textures;
  std::vector<SamplerSlot> samplers;
  std::array<uint64_t, 4> float_constants;  // like ConstantRegisterMap::float_bitmap
  uint32_t bool_constants_used;             // OR of the bool bitmap words
  uint32_t loop_constants_used;
};

// nullopt if equal, otherwise the first reason for the difference.
std::optional<std::string> LayoutMismatch(const ShaderLayout& expected, const ShaderLayout& actual);

struct ShaderOverride {
  uint64_t ucode_hash;
  ShaderLayout layout;
  std::span<const uint8_t> dxbc;
};

class ShaderOverrides {
 public:
  void Add(ShaderOverride o) { by_hash_[o.ucode_hash] = std::move(o); }
  // Valid while the registry lives and no other Add is made.
  const ShaderOverride* Find(uint64_t ucode_hash) const {
    auto it = by_hash_.find(ucode_hash);
    return it == by_hash_.end() ? nullptr : &it->second;
  }
  // True only the first time for (hash, modification): used to log a single line.
  // Thread-safe: Xenia translates from several threads.
  bool FirstReport(uint64_t ucode_hash, uint64_t modification) const;

 private:
  std::unordered_map<uint64_t, ShaderOverride> by_hash_;
  mutable std::mutex mutex_;
  mutable std::set<std::pair<uint64_t, uint64_t>> reported_;
};

// Layout of a pixel shader translation just made by Xenia.
ShaderLayout LayoutOf(const xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov, bool bindless);

// If the registry has the shader's hash: with the same layout it replaces the translated
// DXBC, otherwise it keeps Xenia's translation; one [gpu] log line
// per (hash, modification) in both cases.
void ApplyOverride(const ShaderOverrides& registry, xe::gpu::d3d12::D3D12Shader::D3D12Translation& t, bool rov,
                   bool bindless);

// Replacements compiled into the runtime (runtime/gpu/shaders): DOF downsample.
const ShaderOverrides& BuiltinShaderOverrides();

}  // namespace gpu
