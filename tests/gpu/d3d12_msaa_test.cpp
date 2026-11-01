// Sample 0 of 2x MSAA like the console (thirdparty/xenia patch 0009,
// 2x draws shifted by -0.5 px in y): the depth resolved from sample 0 of a 2x target is the one
// at the pixel center when the viewport is shifted by (-0.25, +0.25) px,
// as the 360's D3D runtime does (Gears of War 2: prepass in EDRAM 2x D24FS8,
// depth resolve from sample 0, GEQUAL lights at the pixel center).
//
// 64x64 2x MSAA surface, k_8_8_8_8 color at base 0, D24FS8 depth at base
// 512; viewport scale (32, -32), offset (32 - 0.25, 32 + 0.25), clip active;
// a triangle covering the whole surface with depth on the plane
// z = 0.25 + 0.0005 x + 0.002 y (x, y in unshifted pixels), written with
// ALWAYS; depth resolve (copy_src_select 4, sample 0) into k_24_8_FLOAT,
// endian kNone. For every interior pixel the float24 depth (high 24 bits of the
// DWORD) equals the plane at the pixel center within 1 float24 ULP. With Xenia's
// sample 0 at (-0.25, -0.25) the error is 0.5 px x 0.002.
//
// Two cases (one backend per process): 2x only, and the 2x depth born from a
// transfer from 1x at the same base (rectangle stencil clear,
// the case that discards programmable positions on an RTX 4080).
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include "d3d12_harness.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "ucode_builder.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"

namespace {

using namespace std::chrono_literals;
using gow2_test::D3D12Harness;
using gow2_test::Pm4Builder;
namespace ub = gow2::ucode_builder;

constexpr uint32_t kSize = 64;
constexpr uint32_t kBytes = kSize * kSize * 4;
constexpr uint32_t kDepthBase = 512;              // EDRAM tile
constexpr uint32_t kRegPaClVportXscale = 0x210F;  // .. XOFFSET, YSCALE, YOFFSET
constexpr uint32_t kPrimTriangleList = 4;
constexpr uint32_t kColorFormat24_8Float = 23;  // ColorFormat::k_24_8_FLOAT
constexpr float kA = 0.0005f, kB = 0.002f, kC = 0.25f;

uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }
float Plane(float x, float y) { return kC + kA * x + kB * y; }

bool HasHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) return false;
  UNSCOPED_INFO("adapter: " << *adapter);
  return true;
}

// Xenos 20e4 float code (4-bit exponent with bias 15, 20-bit
// mantissa) of the normal value v, rounded: consecutive codes are 1 ULP apart.
int64_t Float24Code(double v) {
  int e;
  const double m = std::frexp(v, &e);  // v = m * 2^e, m in [0.5, 1)
  return int64_t(e - 1 + 15) << 20 | int64_t(std::floor((2 * m - 1) * 1048576.0 + 0.5));
}

void DepthFromSample0(bool transfer_from_1x) {
  D3D12Harness h("rtv");
  const uint32_t resolve_vertices = D3D12Harness::AllocPhys(0x1000);
  const uint32_t vertices = D3D12Harness::AllocPhys(0x40000, 0x40000);
  const uint32_t dest = D3D12Harness::AllocPhys(kBytes);
  REQUIRE((resolve_vertices && vertices && dest));
  for (uint32_t off = 0; off < kBytes; off += 4) D3D12Harness::HostPut32(dest + off, 0xDEADBEEF);

  // Triangle beyond the edges (x, y unshifted) -> NDC: x/32 - 1, 1 - y/32.
  const float tri[3][2] = {{-64.f, -64.f}, {192.f, -64.f}, {-64.f, 192.f}};
  for (int i = 0; i < 3; ++i) {
    const float x = tri[i][0], y = tri[i][1];
    const float v[3] = {x / 32.f - 1.f, 1.f - y / 32.f, Plane(x, y)};
    for (int j = 0; j < 3; ++j) D3D12Harness::Put32(vertices + (i * 3 + j) * 4, F(v[j]));
  }

  Pm4Builder p(resolve_vertices);
  // transfer_from_1x: first the same triangle in 1x at the same base, so
  // the 2x target is born from an ownership transfer from the 1x depth
  // (rectangle stencil clear on the 2x depth).
  p.SetSurface(kSize, transfer_from_1x ? gow2_test::kMsaa1X : gow2_test::kMsaa2X, gow2_test::kColorRt8888, 0)
      .ScreenSpaceDrawState()
      .SetRegs(gow2_test::kRegRbDepthInfo, {kDepthBase | 1u << 16 /* kD24FS8 */})
      // z_enable, z_write_enable, zfunc kAlways; blend ONE/ZERO.
      .SetRegs(gow2_test::kRegRbDepthcontrol, {0x2u | 0x4u | 7u << 4, 0x00010001, 0})
      .SetRegs(gow2_test::kRegPaClClipCntl, {1u << 19 /* dx_clip_space_def: z in [0, w] */})
      // Viewport scale and offset in x and y, vtx_xy_fmt, vtx_z_fmt, vtx_w0_fmt.
      .SetRegs(gow2_test::kRegPaClVteCntl, {0xFu | 1u << 8 | 1u << 9 | 1u << 10})
      .SetRegs(kRegPaClVportXscale, {F(32.f), F(32.f - 0.25f), F(-32.f), F(32.f + 0.25f)})
      .ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(vertices, 9)
      .DrawIndx2Auto(kPrimTriangleList, 3);
  if (transfer_from_1x)
    p.SetSurface(kSize, gow2_test::kMsaa2X, gow2_test::kColorRt8888, 0)
        .SetRegs(gow2_test::kRegRbDepthInfo, {kDepthBase | 1u << 16})
        .DrawIndx2Auto(kPrimTriangleList, 3);
  p.ResolveRect(0, 0, kSize, kSize)
      .Resolve(dest, kSize, kSize, gow2_test::kEndianNone, 0, false, kColorFormat24_8Float, false, 4);
  REQUIRE(h.Run(p, 10000ms));

  // 64x64 tiled 32 bpp texture (texture_address::Tiled2D, pitch 64).
  auto tiled_offset = [](uint32_t x, uint32_t y) {
    return uint32_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), 64, 2));
  };
  uint32_t bad = 0;
  std::string first;
  for (uint32_t y = 1; y + 1 < kSize; ++y) {
    for (uint32_t x = 1; x + 1 < kSize; ++x) {
      const uint32_t dword = D3D12Harness::HostGet32(dest + tiled_offset(x, y));
      const int64_t got = dword >> 8;
      const double expected = Plane(x + 0.5f, y + 0.5f);
      if (std::abs(got - Float24Code(expected)) > 1) {
        if (!bad)
          first = "(" + std::to_string(x) + "," + std::to_string(y) + "): code 0x" + std::format("{:06X}", got) +
                  " instead of " + std::to_string(expected) + " (0x" + std::format("{:06X}", Float24Code(expected)) +
                  ")";
        ++bad;
      }
    }
  }
  INFO((transfer_from_1x ? "after the transfer" : "2x only") << ": " << bad << " pixels off by more than 1 float24 ULP, the first " << first);
  CHECK(bad == 0);

  const std::string log = h.StopAndReadLog();
  INFO(log);
  CHECK(log.find("device removed") == std::string::npos);
  CHECK(log.find("[gpu] fatal") == std::string::npos);
}

// Patch 0017: the XDK's Clear draws a rectangle (RECTLIST, 3 vertices, no pixel shader) with MSAA
// forced to 4x on the depth that a target with fewer samples already owns at the same base: with
// gow2_clear_msaa_remap the draw goes to the owner's MSAA (scaled viewport) and transfers nothing.
// Owner: 64x64 1x (or 2x) surface, depth at kDepthBase written at 0.25 with stencil 0x11.
// Clear: 32 px surface at 4x (same pitch in tiles), viewport (w, -32) with offset (w - 0.25, 32.25)
// like the 360's D3D, z 0.75. Then a 1x draw that reads the depth without writing it (brings it back to the 1x owner)
// and the depth resolve: depth and stencil of every pixel, and the number of EDRAM transfers.
// screen_space: like the XDK's real Clear, viewport off (PA_CL_VTE_CNTL without scale or offset), clip
// off and rectangle in 4x pixels from (0, 0) to (rect_width, 32); D3D pixel center (half pixel) or OpenGL.
// Expected clean 1x pixels: columns [cleared_x0, cleared_x1) from row cleared_y0, the others stay the owner's.
struct ClearCase {
  bool remap = true;
  uint32_t owner_msaa = gow2_test::kMsaa1X;
  uint32_t owner_depth_format = 1;  // kD24FS8 (0 = kD24S8)
  float vp_half_width = 32.f;       // 32: the rectangle overflows the 4x surface's 32 px
  uint32_t depthcontrol = 0x8777;   // depth and stencil ALWAYS, write, stencil REPLACE
  bool color_write = false;
  bool screen_space = false;
  float rect_width = 32.f;
  bool d3d_center = false;
  uint32_t cleared_x0 = 0, cleared_x1 = kSize, cleared_y0 = 0;
};
constexpr uint32_t kRegRbStencilRefMask = 0x210D;
constexpr uint32_t kEdramModeDepthOnly = 5;
constexpr uint32_t kOwnerStencil = 0x11, kClearStencil = 0x5A;
constexpr float kOwnerZ = 0.25f, kClearZ = 0.75f;

struct ClearResult {
  int64_t transfers;
  uint32_t bad_depth, bad_stencil;
};

ClearResult ClearOnLowerMsaaOwner(const ClearCase& c, float expected_z, uint32_t expected_stencil) {
  D3D12Harness h("rtv");
  REQUIRE(gpu::SetXeniaCvar("gow2_clear_msaa_remap", c.remap ? "true" : "false"));
  const uint32_t resolve_vertices = D3D12Harness::AllocPhys(0x1000);
  const uint32_t vertices = D3D12Harness::AllocPhys(0x1000);
  const uint32_t dest = D3D12Harness::AllocPhys(kBytes);
  REQUIRE((resolve_vertices && vertices && dest));
  // Triangle covering the surface (NDC, the square [-1, 1] lies under the hypotenuse x - y = 4) at kOwnerZ,
  // then the clear rectangle at kClearZ.
  const float tri[9] = {-1.f, 1.f, kOwnerZ, 5.f, 1.f, kOwnerZ, -1.f, -5.f, kOwnerZ};
  const float ndc_rect[9] = {-1.f, 1.f, kClearZ, 1.f, 1.f, kClearZ, 1.f, -1.f, kClearZ};
  const float screen_rect[9] = {0.f, 0.f, kClearZ, c.rect_width, 0.f, kClearZ, c.rect_width, 32.f, kClearZ};
  const float* rect = c.screen_space ? screen_rect : ndc_rect;
  for (int i = 0; i < 9; ++i) {
    D3D12Harness::Put32(vertices + i * 4, F(tri[i]));
    D3D12Harness::Put32(vertices + 0x100 + i * 4, F(rect[i]));
  }
  const int64_t before = D3D12Harness::SummaryValue("gpu_edram_transfers");

  Pm4Builder p(resolve_vertices);
  auto depth_state = [&](uint32_t depthcontrol, uint32_t stencil_ref) {
    p.SetRegs(gow2_test::kRegRbDepthcontrol, {depthcontrol, 0x00010001, 0})
        .SetRegs(kRegRbStencilRefMask, {stencil_ref | 0xFFu << 8 | 0xFFu << 16})
        .SetRegs(gow2_test::kRegPaClClipCntl, {1u << 19})
        .SetRegs(gow2_test::kRegPaClVteCntl, {0xFu | 1u << 8 | 1u << 9 | 1u << 10});
  };
  // Owner: depth kOwnerZ and stencil kOwnerStencil everywhere.
  p.SetSurface(kSize, c.owner_msaa, gow2_test::kColorRt8888, 0)
      .ScreenSpaceDrawState()
      .SetRegs(gow2_test::kRegRbDepthInfo, {kDepthBase | c.owner_depth_format << 16});
  depth_state(0x8777, kOwnerStencil);
  p.SetRegs(kRegPaClVportXscale, {F(32.f), F(32.f), F(-32.f), F(32.f)})
      .ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(vertices, 9)
      .DrawIndx2Auto(kPrimTriangleList, 3);
  // 4x clear: without pixel shader (kDepthOnly) or with color writes.
  p.SetSurface(kSize / 2, gow2_test::kMsaa4X, gow2_test::kColorRt8888, 0)
      .SetRegs(gow2_test::kRegRbDepthInfo, {kDepthBase | 1u << 16})
      .SetRegs(gow2_test::kRegRbModecontrol, {c.color_write ? gow2_test::kEdramModeColorDepth : kEdramModeDepthOnly})
      .SetRegs(gow2_test::kRegRbColorMask, {c.color_write ? 0xFu : 0u});
  depth_state(c.depthcontrol, kClearStencil);
  p.SetRegs(kRegPaClVportXscale, {F(c.vp_half_width), F(c.vp_half_width - 0.25f), F(-32.f), F(32.25f)});
  if (c.screen_space)
    p.SetRegs(gow2_test::kRegPaClClipCntl, {1u << 16})
        .SetRegs(gow2_test::kRegPaClVteCntl, {1u << 8 | 1u << 9 | 1u << 10})
        .SetRegs(gow2_test::kRegPaSuVtxCntl, {c.d3d_center ? 0u : 1u});
  p.VertexFetch0(vertices + 0x100, 9)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  // 1x draw that reads the depth (z ALWAYS without writing), then the depth resolve.
  p.SetSurface(kSize, gow2_test::kMsaa1X, gow2_test::kColorRt8888, 0)
      .ScreenSpaceDrawState()
      .SetRegs(gow2_test::kRegRbDepthInfo, {kDepthBase | 1u << 16});
  depth_state(0x2u | 7u << 4, 0);
  p.SetRegs(kRegPaClVportXscale, {F(32.f), F(32.f), F(-32.f), F(32.f)})
      .VertexFetch0(vertices, 9)
      .DrawIndx2Auto(kPrimTriangleList, 3)
      .ResolveRect(0, 0, kSize, kSize)
      .Resolve(dest, kSize, kSize, gow2_test::kEndianNone, 0, false, kColorFormat24_8Float, false, 4);
  REQUIRE(h.Run(p, 10000ms));

  ClearResult r{D3D12Harness::SummaryValue("gpu_edram_transfers") - before, 0, 0};
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      const uint32_t dword = D3D12Harness::HostGet32(
          dest + uint32_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), kSize, 2)));
      const bool cleared = x >= c.cleared_x0 && x < c.cleared_x1 && y >= c.cleared_y0;
      if (std::abs(int64_t(dword >> 8) - Float24Code(cleared ? expected_z : kOwnerZ)) > 1) ++r.bad_depth;
      if ((dword & 0xFF) != (cleared ? expected_stencil : kOwnerStencil)) ++r.bad_stencil;
    }
  }
  const std::string log = h.StopAndReadLog();
  INFO(log);
  CHECK(log.find("device removed") == std::string::npos);
  CHECK(log.find("[gpu] fatal") == std::string::npos);
  return r;
}

}  // namespace

TEST_CASE("d3d12_msaa: 4x depth and stencil clear on a 1x owner, remapped without transfers",
          "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  const ClearResult r = ClearOnLowerMsaaOwner({}, kClearZ, kClearStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

// Without the cvar the clear goes through the 4x target with a transfer. Values not checked: the top edge at
// 0.25 px with the standard host sample positions leaves part of the first row as it was, while the
// XDK's rectangle covers the whole surface (the remap cleans all of it).
TEST_CASE("d3d12_msaa: 4x depth and stencil clear without remap, with transfers", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.remap = false;
  CHECK(ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil).transfers > 0);
}

TEST_CASE("d3d12_msaa: 4x clear in screen coordinates (viewport off) remapped, whole surface",
          "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.screen_space = true;
  const ClearResult r = ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

// Integer edges with the OpenGL center: the 4 samples of every 4x pixel are on the same side, 4x pixels 0..15
// are 1x columns 0..31.
TEST_CASE("d3d12_msaa: partial 4x clear in screen coordinates remapped, OpenGL pixel center",
          "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.screen_space = true;
  c.rect_width = 16.f;
  c.cleared_x1 = 32;
  const ClearResult r = ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

// D3D center: Xenia shifts the positions by half a pixel, the 4x edges go to 0.5 and 16.5 (x) and from 0.5 (y) and,
// doubled, to 1 and 33 and from 1 in 1x pixels: columns 1..32 from row 1 (the samples before the edge stay).
TEST_CASE("d3d12_msaa: partial 4x clear in screen coordinates remapped, D3D pixel center",
          "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.screen_space = true;
  c.rect_width = 16.f;
  c.d3d_center = true;
  c.cleared_x0 = 1;
  c.cleared_x1 = 33;
  c.cleared_y0 = 1;
  const ClearResult r = ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

TEST_CASE("d3d12_msaa: 4x stencil-only clear remapped, the owner's depth stays", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.depthcontrol = 0x8701;  // stencil only ALWAYS REPLACE
  const ClearResult r = ClearOnLowerMsaaOwner(c, kOwnerZ, kClearStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

TEST_CASE("d3d12_msaa: 4x depth-only clear remapped, the owner's stencil stays", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.depthcontrol = 0x0076;  // depth only ALWAYS with write
  const ClearResult r = ClearOnLowerMsaaOwner(c, kClearZ, kOwnerStencil);
  CHECK(r.transfers == 0);
  CHECK(r.bad_depth == 0);
  CHECK(r.bad_stencil == 0);
}

// Cases left to the normal path (transfers as before): viewport that does not overflow the surface,
// owner with another depth format, 2x owner, clear that also writes color.
TEST_CASE("d3d12_msaa: 4x clear not remapped, narrow viewport", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.vp_half_width = 16.f;
  CHECK(ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil).transfers > 0);
}

TEST_CASE("d3d12_msaa: 4x clear not remapped, D24S8 owner", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.owner_depth_format = 0;
  CHECK(ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil).transfers > 0);
}

TEST_CASE("d3d12_msaa: 4x clear not remapped, 2x owner", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.owner_msaa = gow2_test::kMsaa2X;
  CHECK(ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil).transfers > 0);
}

TEST_CASE("d3d12_msaa: 4x clear not remapped, color write", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearCase c;
  c.color_write = true;
  CHECK(ClearOnLowerMsaaOwner(c, kClearZ, kClearStencil).transfers > 0);
}

TEST_CASE("d3d12_msaa: 2x depth from sample 0 at the pixel center", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DepthFromSample0(false);
}

TEST_CASE("d3d12_msaa: 2x depth from sample 0 after a transfer from 1x", "[d3d12_msaa]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DepthFromSample0(true);
}

// Patch 0009 with the internal resolution: host sample 0 sits at (-0.25,
// -0.25) host px from the host pixel center at every scale, the game's shift
// is (-0.25, +0.25) guest px; the added shift, in host px, is
// (0.25 sx - 0.25, -0.25 sy - 0.25). Clip disabled and origin at the top: ndc_offset * extent / 2
// is the offset in host px. CPU only, no adapter.
TEST_CASE("draw_util: shift of 2x draws toward the console's sample 0, per scale", "[d3d12_msaa]") {
  using namespace xe::gpu;
  struct Case { uint32_t sx, sy; float dx, dy; };
  const Case cases[] = {{1, 1, 0.0f, -0.5f}, {2, 2, 0.25f, -0.75f}, {3, 3, 0.5f, -1.0f}, {2, 1, 0.25f, -0.5f}};
  RegisterFile regs;
  std::fill(std::begin(regs.values), std::end(regs.values), 0u);
  reg::PA_CL_CLIP_CNTL clip{};
  clip.clip_disable = 1;
  regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL] = clip.value;
  reg::RB_SURFACE_INFO surface{};
  surface.msaa_samples = xenos::MsaaSamples::k2X;
  regs.values[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  // Viewport offset at the center (kSize / 2): ndc_offset is just the shift, without rounding.
  reg::PA_CL_VTE_CNTL vte{};
  vte.vport_x_offset_ena = 1;
  vte.vport_y_offset_ena = 1;
  regs.values[XE_GPU_REG_PA_CL_VTE_CNTL] = vte.value;
  regs.values[XE_GPU_REG_PA_CL_VPORT_XOFFSET] = F(kSize / 2.0f);
  regs.values[XE_GPU_REG_PA_CL_VPORT_YOFFSET] = F(kSize / 2.0f);
  for (const Case& c : cases) {
    INFO("scale " << c.sx << "x" << c.sy);
    draw_util::ViewportInfo vp[2];
    for (int compensate = 0; compensate < 2; ++compensate) {
      draw_util::GetViewportInfoArgs args{};
      args.Setup(c.sx, c.sy, xe::divisors::MagicDiv(c.sx), xe::divisors::MagicDiv(c.sy), false, kSize * c.sx,
                 kSize * c.sy, false, reg::RB_DEPTHCONTROL{}, false, false, false, false);
      args.SetupRegisterValues(regs, false);
      args.msaa_2x_sample_0_to_console = compensate;
      draw_util::GetHostViewportInfo(&args, vp[compensate]);
    }
    for (int i = 0; i < 2; ++i) {
      REQUIRE(vp[0].xy_extent[i] == vp[1].xy_extent[i]);
      const float px = (vp[1].ndc_offset[i] - vp[0].ndc_offset[i]) * float(vp[0].xy_extent[i]) * 0.5f;
      INFO("axis " << i << ": " << px);
      CHECK(std::abs(px - (i ? c.dy : c.dx)) < 1e-6f);
    }
  }
}

// Patch 0013: the upscaler jitter (gow2_jitter_xy, guest px) shifts the viewport by jitter * scale host px,
// in addition to 0009; jitter (0, 0) leaves the viewport identical. CPU only.
TEST_CASE("draw_util: upscaler jitter in guest px, per scale", "[d3d12_msaa]") {
  using namespace xe::gpu;
  RegisterFile regs;
  std::fill(std::begin(regs.values), std::end(regs.values), 0u);
  reg::PA_CL_CLIP_CNTL clip{};
  clip.clip_disable = 1;
  regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL] = clip.value;
  reg::RB_SURFACE_INFO surface{};
  surface.msaa_samples = xenos::MsaaSamples::k2X;
  regs.values[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  reg::PA_CL_VTE_CNTL vte{};
  vte.vport_x_offset_ena = 1;
  vte.vport_y_offset_ena = 1;
  regs.values[XE_GPU_REG_PA_CL_VTE_CNTL] = vte.value;
  regs.values[XE_GPU_REG_PA_CL_VPORT_XOFFSET] = F(kSize / 2.0f);
  regs.values[XE_GPU_REG_PA_CL_VPORT_YOFFSET] = F(kSize / 2.0f);
  for (uint32_t s : {1u, 2u, 3u}) {
    INFO("scale " << s);
    auto viewport = [&](float jx, float jy) {
      draw_util::GetViewportInfoArgs args{};
      args.Setup(s, s, xe::divisors::MagicDiv(s), xe::divisors::MagicDiv(s), false, kSize * s, kSize * s, false,
                 reg::RB_DEPTHCONTROL{}, false, false, false, false);
      args.SetupRegisterValues(regs, false);
      args.msaa_2x_sample_0_to_console = 1;
      args.gow2_jitter_xy[0] = jx;
      args.gow2_jitter_xy[1] = jy;
      draw_util::ViewportInfo vp;
      draw_util::GetHostViewportInfo(&args, vp);
      return vp;
    };
    const auto base = viewport(0.0f, 0.0f), jittered = viewport(0.25f, -0.125f);
    for (int i = 0; i < 2; ++i) {
      const float px = (jittered.ndc_offset[i] - base.ndc_offset[i]) * float(base.xy_extent[i]) * 0.5f;
      INFO("axis " << i << ": " << px);
      CHECK(std::abs(px - (i ? -0.125f : 0.25f) * float(s)) < 1e-5f);
    }
  }
}

// With canary's Xenos positions (xenos_sample_positions, not used by gow2) the fractional part of the offset
// that coincides with a sample stays in host px: the jitter must not break that correction, it always shifts
// by jitter * scale host px. CPU only.
TEST_CASE("draw_util: upscaler jitter with the Xenos sample positions", "[d3d12_msaa]") {
  using namespace xe::gpu;
  constexpr uint32_t s = 2;
  RegisterFile regs;
  std::fill(std::begin(regs.values), std::end(regs.values), 0u);
  reg::PA_CL_CLIP_CNTL clip{};
  clip.clip_disable = 1;
  regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL] = clip.value;
  reg::RB_SURFACE_INFO surface{};
  surface.msaa_samples = xenos::MsaaSamples::k2X;
  regs.values[XE_GPU_REG_RB_SURFACE_INFO] = surface.value;
  reg::PA_CL_VTE_CNTL vte{};
  vte.vport_x_offset_ena = 1;
  vte.vport_y_offset_ena = 1;
  regs.values[XE_GPU_REG_PA_CL_VTE_CNTL] = vte.value;
  auto viewport = [&](bool xenos_positions, float jx, float jy) {
    draw_util::GetViewportInfoArgs args{};
    args.Setup(s, s, xe::divisors::MagicDiv(s), xe::divisors::MagicDiv(s), false, kSize * s, kSize * s, false,
               reg::RB_DEPTHCONTROL{}, false, false, false, xenos_positions);
    args.SetupRegisterValues(regs, false);
    args.gow2_jitter_xy[0] = jx;
    args.gow2_jitter_xy[1] = jy;
    draw_util::ViewportInfo vp;
    draw_util::GetHostViewportInfo(&args, vp);
    return vp;
  };
  // Fractional offset on sample 0 of 2x (-4/16, +4/16): with or without Xenia's half pixel, one of the two.
  bool corrected = false;
  for (const float frac : {0.25f, 0.75f}) {
    regs.values[XE_GPU_REG_PA_CL_VPORT_XOFFSET] = F(kSize / 2.0f + frac);
    regs.values[XE_GPU_REG_PA_CL_VPORT_YOFFSET] = F(kSize / 2.0f + 1.0f - frac);
    const auto plain = viewport(false, 0.0f, 0.0f), base = viewport(true, 0.0f, 0.0f);
    if (plain.ndc_offset[0] == base.ndc_offset[0]) continue;  // correction not applied to this offset
    corrected = true;
    const auto jittered = viewport(true, 0.25f, -0.125f);
    for (int i = 0; i < 2; ++i) {
      const float px = (jittered.ndc_offset[i] - base.ndc_offset[i]) * float(base.xy_extent[i]) * 0.5f;
      INFO("axis " << i << ": " << px);
      CHECK(std::abs(px - (i ? -0.125f : 0.25f) * float(s)) < 1e-5f);
    }
  }
  REQUIRE(corrected);
}

// Which draws get the upscaler jitter, and by how much of it (patch 0013): the scene surfaces (pitch of the
// scene) the whole jitter; the occlusion query boxes (depth test without depth or color writes on the
// half-resolution surface) half of it in their own pixels, so they stay on the jittered geometry the query
// depth was built from (GoW3 distant floors flashing with DLSS); depth restores, remapped clears, shadow maps
// and other half-resolution passes none. CPU only.
TEST_CASE("draw_util: upscaler jitter scale per draw", "[d3d12_msaa]") {
  using xe::gpu::draw_util::Gow2JitterScale;
  using xe::gpu::xenos::CompareFunction;
  constexpr uint32_t kScene = 1280, kHalf = 640;
  // Scene geometry and lights.
  CHECK(Gow2JitterScale(true, true, CompareFunction::kGreaterEqual, false, kScene, kScene, 0xF, false) == 1.0f);
  CHECK(Gow2JitterScale(true, false, CompareFunction::kGreaterEqual, false, kScene, kScene, 0x0, false) == 1.0f);
  // Query boxes on the half-resolution query depth.
  CHECK(Gow2JitterScale(true, false, CompareFunction::kGreaterEqual, false, kHalf, kScene, 0x0, false) == 0.5f);
  // Half-resolution passes writing color or depth (velocity, the query depth restore itself).
  CHECK(Gow2JitterScale(true, false, CompareFunction::kGreaterEqual, false, kHalf, kScene, 0xF, false) == 0.0f);
  CHECK(Gow2JitterScale(true, true, CompareFunction::kAlways, true, kHalf, kScene, 0x0, false) == 0.0f);
  // Scene depth restore, remapped clear, no depth test, shadow map, no scene pitch yet.
  CHECK(Gow2JitterScale(true, true, CompareFunction::kAlways, true, kScene, kScene, 0x0, false) == 0.0f);
  CHECK(Gow2JitterScale(true, true, CompareFunction::kGreaterEqual, false, kScene, kScene, 0xF, true) == 0.0f);
  CHECK(Gow2JitterScale(false, false, CompareFunction::kAlways, false, kScene, kScene, 0xF, false) == 0.0f);
  CHECK(Gow2JitterScale(true, true, CompareFunction::kLessEqual, false, 1024, kScene, 0x0, false) == 0.0f);
  CHECK(Gow2JitterScale(true, false, CompareFunction::kGreaterEqual, false, 0, 0, 0x0, false) == 1.0f);
}
