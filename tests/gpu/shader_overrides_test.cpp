// Registry of replacement shaders and layout check (gpu/shader_overrides.h), CPU only.
#include <catch2/catch_test_macros.hpp>

#include "gpu/shader_overrides.h"

namespace {

gpu::ShaderLayout Base() {
  gpu::ShaderLayout l{};
  l.bindless = true;
  l.textures.push_back({0, 1, 2, false});
  l.samplers.push_back({0, 1, 0, 0, 0, 0});
  return l;
}

}  // namespace

TEST_CASE("shader_overrides: equal layout accepted", "[shader_overrides]") {
  CHECK(gpu::LayoutMismatch(Base(), Base()) == std::nullopt);
}

TEST_CASE("shader_overrides: every difference has its reason", "[shader_overrides]") {
  const auto base = Base();
  auto check = [&](auto mutate, const char* reason) {
    auto l = Base();
    mutate(l);
    CHECK(gpu::LayoutMismatch(base, l) == std::optional<std::string>(reason));
  };
  check([](gpu::ShaderLayout& l) { l.rov = true; }, "ROV path");
  check([](gpu::ShaderLayout& l) { l.bindless = false; }, "non-bindless resources");
  check([](gpu::ShaderLayout& l) { l.interpolator_mask = 3; }, "interpolators");
  check([](gpu::ShaderLayout& l) { l.textures[0].descriptor_index++; }, "texture");
  check([](gpu::ShaderLayout& l) { l.samplers[0].fetch_constant++; }, "sampler");
  check([](gpu::ShaderLayout& l) { l.float_constants[0] |= 1ull << 7; }, "float constants");
  check([](gpu::ShaderLayout& l) { l.bool_constants_used = 1; }, "bool/loop constants");
}

TEST_CASE("shader_overrides: lookup by hash and one line per hash and modification", "[shader_overrides]") {
  constexpr uint64_t h = 0x0354AF5BA0F333D1ull;
  gpu::ShaderOverrides reg;
  reg.Add({h, Base(), {}});
  CHECK(reg.Find(h) != nullptr);
  CHECK(reg.Find(1) == nullptr);
  CHECK(reg.FirstReport(h, 1));
  CHECK_FALSE(reg.FirstReport(h, 1));
  CHECK(reg.FirstReport(h, 2));
}
