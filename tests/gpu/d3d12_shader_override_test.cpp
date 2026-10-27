// Replacement shaders (gpu/shader_overrides.h, Xenia hook of vendor
// patch 0010) on the --gpu d3d12 backend with a synthetic PM4 ring
// (tests/gpu/d3d12_harness.h, rtv path, bindless resources).
//
// The guest pixel shader is synthetic (tests/gpu/ucode_builder.h) but has the
// layout of the game's DOF downsample: tfetch2D r1, r0.xy, tf1, OffsetX=3 first
// (the order of first use decides the bindless slots), then tfetch2D r2,
// r0.xy, tf0; a chain of max reading c2, c3, c4, c5, c6 and c255;
// export oC0 = r1. The test hook, at the first translation, registers under
// the ucode hash computed by Xenia the layout of that translation
// (LayoutOf) with the DXBC of runtime/gpu/shaders/dof_downsample_4x4.hlsl, then
// calls ApplyOverride. No game bytes.
//
// Scene: tf0 depth k_32_FLOAT 16x16, tf1 color k_32_32_32_32_FLOAT
// 16x16 (tiled, endian k8in32, point filter), fixed pseudo-random values;
// 4x4 k_16_16_16_16_FLOAT render target (Xenos has no 4-component 32-bit
// render targets): pixel (x, y) has the texcoord at the center of texel
// (4x, 4y), top-left corner of its 4x4 block. Resolve toward a
// k_16_16_16_16_FLOAT destination, endian k8in16.
//
//  - active replacement: every pixel = CPU downsample formula
//    (Formula, same math as formula() in the spike's ucode_sim.py),
//    tolerance of the render target's half precision; log line
//    "active";
//  - altered expected layout (tf of the first texture with is_signed
//    inverted): Xenia's translation stays, the output is r1's color
//    (texel (4x + 3, 4y)); log line "layout differs ... (texture)".
// Every case is a separate TEST_CASE (one backend per process, like ctest).
// Skipped (SKIP) without a hardware D3D12 adapter.
#include "d3d12_harness.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <sstream>
#include <string>

#include "gpu/d3d12_probe.h"
#include "gpu/shader_overrides.h"
#include "shaders/dof_downsample_4x4.h"
#include "ucode_builder.h"
#include "xenia/gpu/d3d12/pipeline_cache.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/xenos.h"

namespace {

using namespace std::chrono_literals;
using gow2_test::D3D12Harness;
using gow2_test::Pm4Builder;
namespace ub = gow2::ucode_builder;
namespace xenos = xe::gpu::xenos;
using Translation = xe::gpu::d3d12::D3D12Shader::D3D12Translation;

constexpr uint32_t kTex = 16;   // texture 16x16
constexpr uint32_t kOut = 4;    // render target 4x4
constexpr uint32_t kPitch = 32;  // pitch of the surface and the destination
constexpr uint32_t kVertexFetch = 6;  // vf6: textures 0 and 1 cover vf0..vf5
constexpr uint32_t kVertexDwords = 15;
constexpr uint32_t kColorRt16161616Float = 7;     // ColorRenderTargetFormat
constexpr uint32_t kColorFormat16161616Float = 32;  // ColorFormat

uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }

bool HasHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) return false;
  UNSCOPED_INFO("adapter: " << *adapter);
  return true;
}

uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks, unsigned bpb_log2) {
  const uint32_t pitch_aligned = (pitch_blocks + 31) & ~31u;
  return uint32_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), pitch_aligned, bpb_log2));
}

float Half(uint16_t h) {
  const int e = (h >> 10) & 31, m = h & 1023;
  const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m + 1024), e - 25);
  return (h & 0x8000) ? -v : v;
}

// Fixed values in [0, 1.2) (LCG).
struct Values {
  uint32_t state = 12345;
  float Next() {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24) * 1.2f;
  }
};

struct Scene {
  std::array<float, kTex * kTex> depth;
  std::array<std::array<float, 4>, kTex * kTex> color;
  // c2, c3, c4, c5, c6, c255 as in the spike's simulator.
  std::array<float, 4> c2{0.f, 0.f, 1.5f, -0.5f}, c3{1.2f, 0.8f, 1.3f, 0.f}, c4{2.f, 1.5f, 0.f, 0.f},
      c5{0.75f, 0.f, 0.f, 0.f}, c6{1.6f, 0.f, 0.f, 0.f}, c255{1.f, 1.f / 16.f, 1e-4f, 0.f};
  Scene() {
    Values v;
    for (auto& d : depth) d = v.Next();
    for (auto& c : color)
      for (auto& x : c) x = v.Next();
  }
};

// Xenos multiplication: 0 * anything = 0.
double XMul(double a, double b) { return (a == 0 || b == 0) ? 0.0 : a * b; }

// formula() of ucode_sim.py: DOF downsample of the 4x4 block of pixel (px, py).
std::array<double, 4> Formula(const Scene& s, uint32_t px, uint32_t py) {
  std::array<double, 3> sum_color{};
  double sum_weight = 0;
  for (uint32_t ty = 0; ty < 4; ++ty)
    for (uint32_t tx = 0; tx < 4; ++tx) {
      const uint32_t i = (4 * py + ty) * kTex + 4 * px + tx;
      const double d = s.depth[i];
      std::array<double, 3> col;
      for (int c = 0; c < 3; ++c) col[c] = XMul(s.color[i][c], s.c6[0]);
      const double t = 1.0 / (XMul(d, s.c2[2]) - s.c2[3]) - s.c3[0];
      const double near_far = 0 < -t ? s.c4[0] : s.c4[1];
      const double p = std::max(std::abs(XMul(t, s.c3[1])), double(s.c255[2]));
      const double w = std::min(near_far, std::exp2(XMul(std::log2(std::abs(p)), s.c3[2])));
      const bool bright = std::ranges::any_of(col, [&](double c) { return c >= s.c255[0]; });
      for (int c = 0; c < 3; ++c) sum_color[c] += XMul(col[c], XMul(bright ? 1.0 : 0.0, s.c5[0]) + w);
      sum_weight += XMul(w, s.c255[0]);
    }
  return {XMul(sum_color[0], s.c255[1]), XMul(sum_color[1], s.c255[1]), XMul(sum_color[2], s.c255[1]),
          XMul(sum_weight, s.c255[1])};
}

std::string Describe(const gpu::ShaderLayout& l) {
  std::ostringstream o;
  o << "rov=" << l.rov << " bindless=" << l.bindless << " interpolator_mask=" << l.interpolator_mask
    << " centroid=" << l.interpolators_centroid << " param_gen=" << l.param_gen << "\n";
  for (const auto& t : l.textures)
    o << "  texture {" << t.descriptor_index << ", " << t.fetch_constant << ", " << t.dimension << ", "
      << t.is_signed << "}\n";
  for (const auto& s : l.samplers)
    o << "  sampler {" << s.descriptor_index << ", " << s.fetch_constant << ", " << s.mag_filter << ", "
      << s.min_filter << ", " << s.mip_filter << ", " << s.aniso_filter << "}\n";
  o << std::hex << "  float {0x" << l.float_constants[0] << ", 0x" << l.float_constants[1] << ", 0x"
    << l.float_constants[2] << ", 0x" << l.float_constants[3] << "} bool 0x" << l.bool_constants_used
    << " loop 0x" << l.loop_constants_used;
  return o.str();
}

// Test hook state (function pointer without capture).
gpu::ShaderOverrides* g_registry = nullptr;
bool g_alter_layout = false;
std::optional<uint64_t> g_hash;
std::optional<gpu::ShaderLayout> g_layout;

// At the first translation registers the shader under its own hash, then
// applies the registry as the runtime will.
void TestHook(Translation& t, bool rov, bool bindless) {
  if (!g_hash) {
    g_hash = t.shader().ucode_data_hash();
    g_layout = gpu::LayoutOf(t, rov, bindless);
    gpu::ShaderLayout expected = *g_layout;
    if (g_alter_layout && !expected.textures.empty()) expected.textures[0].is_signed ^= true;
    g_registry->Add({*g_hash, expected, kDofDownsample4x4Dxbc});
  }
  gpu::ApplyOverride(*g_registry, t, rov, bindless);
}

// The hook is global in Xenia: it must be removed at the end of the test (the TEST_CASEs
// share the process). Declared before the fixture: removed after
// gpu::Shutdown, when nobody translates any more.
struct HookGuard {
  explicit HookGuard(gpu::ShaderOverrides& registry, bool alter) {
    g_registry = &registry;
    g_alter_layout = alter;
    g_hash.reset();
    g_layout.reset();
    xe::gpu::d3d12::SetTranslatedPixelShaderHook(&TestHook);
  }
  ~HookGuard() {
    xe::gpu::d3d12::SetTranslatedPixelShaderHook(nullptr);
    g_registry = nullptr;
  }
};

// Synthetic PS with the layout of the DOF downsample; oC0 = r1.
std::vector<uint32_t> PsDofLayout() {
  using ub::AluSrc;
  using ub::C;
  using ub::R;
  using xe::gpu::ucode::AluVectorOpcode;
  constexpr uint32_t first = ub::FirstInstr(3);
  return ub::Assemble(
      {ub::CfAlloc(ub::AllocType::kPsColors, 0),
       ub::CfExec(false, first, 6,
                  ub::Seq({ub::kSeqFetch, ub::kSeqFetch, ub::kSeqAluSerialize, ub::kSeqAlu, ub::kSeqAlu, ub::kSeqAlu})),
       ub::CfExec(true, first + 6, 2, ub::Seq({ub::kSeqAlu, ub::kSeqAlu}))},
      {ub::TextureFetch2D(1, 0, 1, 3), ub::TextureFetch2D(2, 0, 0), ub::AluVector(AluVectorOpcode::kMax, 3, C(2), C(3)),
       ub::AluVector(AluVectorOpcode::kMax, 3, R(3), C(4)), ub::AluVector(AluVectorOpcode::kMax, 3, R(3), C(5)),
       ub::AluVector(AluVectorOpcode::kMax, 3, R(3), C(6)), ub::AluVector(AluVectorOpcode::kMax, 3, R(3), C(255)),
       ub::AluExportMov(ub::ExportRegister::kPSColor0, R(1))});
}

// Fetch constant of a 16x16 tiled 2D texture, point filter, one level.
std::array<uint32_t, 6> TextureFetchConstant(uint32_t tex, xenos::TextureFormat format) {
  xenos::xe_gpu_texture_fetch_t f{};
  f.type = xenos::FetchConstantType::kTexture;
  f.clamp_x = f.clamp_y = f.clamp_z = xenos::ClampMode::kClampToEdge;
  f.pitch = 1;  // in blocks of 32 texels
  f.tiled = 1;
  f.format = format;
  f.endianness = xenos::Endian::k8in32;
  f.base_address = (tex & 0x1FFFFFFF) >> 12;
  f.size_2d.width = kTex - 1;
  f.size_2d.height = kTex - 1;
  f.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  f.mag_filter = f.min_filter = f.mip_filter = xenos::TextureFilter::kPoint;
  f.aniso_filter = xenos::AnisoFilter::kDisabled;
  f.dimension = xenos::DataDimension::k2DOrStacked;
  return {f.dword_0, f.dword_1, f.dword_2, f.dword_3, f.dword_4, f.dword_5};
}

void PutConstant(Pm4Builder& p, uint32_t reg, const std::array<float, 4>& c) {
  p.SetAluConstants(gow2_test::kAluConstantPs0 + reg * 4, {F(c[0]), F(c[1]), F(c[2]), F(c[3])});
}

class OverrideFixture {
 public:
  OverrideFixture() : h_("rtv") {
    depth_ = D3D12Harness::AllocPhys(0x10000);
    color_ = D3D12Harness::AllocPhys(0x10000);
    vertices_ = D3D12Harness::AllocPhys(0x1000);
    resolve_vertices_ = D3D12Harness::AllocPhys(0x1000);
    dest_ = D3D12Harness::AllocPhys(0x10000);
    REQUIRE((depth_ && color_ && vertices_ && resolve_vertices_ && dest_));
    for (uint32_t y = 0; y < kTex; ++y)
      for (uint32_t x = 0; x < kTex; ++x) {
        const uint32_t i = y * kTex + x;
        D3D12Harness::Put32(depth_ + TiledOffset(x, y, kTex, 2), F(scene_.depth[i]));
        for (uint32_t c = 0; c < 4; ++c)
          D3D12Harness::Put32(color_ + TiledOffset(x, y, kTex, 4) + c * 4, F(scene_.color[i][c]));
      }
    // (x, y, z, u, v): (0,0) (4,0) (4,4); u = (x - 0.375) / 4 brings the center
    // of pixel x to the center of texel 4x of the 16x16 texture (same for v).
    const float lo = -0.375f / kOut, hi = (kOut - 0.375f) / kOut, s = float(kOut);
    const uint32_t v[kVertexDwords] = {F(0.f), F(0.f), F(0.f), F(lo), F(lo), F(s),  F(0.f), F(0.f),
                                       F(hi),  F(lo),  F(s),   F(s),  F(0.f), F(hi), F(hi)};
    for (uint32_t i = 0; i < kVertexDwords; ++i) D3D12Harness::Put32(vertices_ + i * 4, v[i]);
  }

  const Scene& scene() const { return scene_; }

  void DrawAndResolve() {
    Pm4Builder p(resolve_vertices_);
    p.SetSurface(kPitch, gow2_test::kMsaa1X, kColorRt16161616Float, 0)
        .ScreenSpaceDrawState()
        .ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionTexcoord(kVertexFetch))
        .ImLoadImmediate(gow2_test::kShaderPixel, PsDofLayout())
        .TextureFetch(0, TextureFetchConstant(depth_, xenos::TextureFormat::k_32_FLOAT))
        .TextureFetch(1, TextureFetchConstant(color_, xenos::TextureFormat::k_32_32_32_32_FLOAT))
        .VertexFetch(kVertexFetch, vertices_, kVertexDwords);
    PutConstant(p, 2, scene_.c2);
    PutConstant(p, 3, scene_.c3);
    PutConstant(p, 4, scene_.c4);
    PutConstant(p, 5, scene_.c5);
    PutConstant(p, 6, scene_.c6);
    PutConstant(p, 255, scene_.c255);
    p.DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
    p.ResolveRect(0, 0, 8, 8)
        .Resolve(dest_, kPitch, kPitch, gow2_test::kEndian8in16, 0, false, kColorFormat16161616Float);
    REQUIRE(h_.Run(p, 10000ms));
    CHECK(D3D12Harness::SummaryValue("gpu_draws_skipped") == 0);
  }

  // Resolved pixel (x, y): 4 big-endian half precisions (k8in16).
  std::array<float, 4> Pixel(uint32_t x, uint32_t y) const {
    const uint32_t at = dest_ + TiledOffset(x, y, kPitch, 3);
    std::array<float, 4> r;
    for (uint32_t c = 0; c < 4; ++c) r[c] = Half(*kernel::GuestPtr<be<uint16_t>>(at + 2 * c));
    return r;
  }

  // Each 4x4 pixel against expected(x, y), relative error (absolute below 1)
  // within the render target's half precision.
  template <class Expected>
  void CheckPixels(Expected expected) const {
    for (uint32_t y = 0; y < kOut; ++y)
      for (uint32_t x = 0; x < kOut; ++x) {
        const auto got = Pixel(x, y);
        const auto want = expected(x, y);
        for (uint32_t c = 0; c < 4; ++c) {
          INFO("pixel (" << x << "," << y << ") component " << c << ": " << got[c] << ", expected " << want[c]);
          CHECK(std::abs(got[c] - want[c]) <= 1e-3 * std::max(1.0, std::abs(double(want[c]))));
        }
      }
  }

  std::string StopAndReadLog() { return h_.StopAndReadLog(); }

 private:
  Scene scene_;
  D3D12Harness h_;
  uint32_t depth_ = 0, color_ = 0, vertices_ = 0, resolve_vertices_ = 0, dest_ = 0;
};

}  // namespace

TEST_CASE("replacement shader: DOF downsample equals the formula on synthetic textures", "[shader_override]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  gpu::ShaderOverrides registry;
  HookGuard hook(registry, false);
  OverrideFixture f;
  f.DrawAndResolve();
  REQUIRE(g_hash);
  INFO("layout of Xenia's translation:\n" << Describe(*g_layout));
  // The game's entry in BuiltinShaderOverrides has this same layout.
  const gpu::ShaderOverride* dof = gpu::BuiltinShaderOverrides().Find(0x0354AF5BA0F333D1ull);
  REQUIRE(dof);
  CHECK(gpu::LayoutMismatch(dof->layout, *g_layout) == std::nullopt);
  f.CheckPixels([&](uint32_t x, uint32_t y) { return Formula(f.scene(), x, y); });
  const std::string log = f.StopAndReadLog();
  INFO(log);
  CHECK(log.find(std::format("[gpu] replacement shader {:016X} active", *g_hash)) != std::string::npos);
  CHECK(log.find("layout differs") == std::string::npos);
  CHECK(log.find("device removed") == std::string::npos);
}

TEST_CASE("replacement shader: different layout, Xenia's translation stays", "[shader_override]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  gpu::ShaderOverrides registry;
  HookGuard hook(registry, true);
  OverrideFixture f;
  f.DrawAndResolve();
  REQUIRE(g_hash);
  // r1 = tfetch2D tf1, OffsetX=3: the texel (4x + 3, 4y).
  f.CheckPixels([&](uint32_t x, uint32_t y) { return f.scene().color[4 * y * kTex + 4 * x + 3]; });
  const std::string log = f.StopAndReadLog();
  INFO(log);
  CHECK(log.find(std::format("[gpu] replacement shader {:016X}: layout differs from the expected one (texture), using Xenia's "
                             "translation",
                             *g_hash)) != std::string::npos);
  CHECK(log.find(std::format("{:016X} active", *g_hash)) == std::string::npos);
}
