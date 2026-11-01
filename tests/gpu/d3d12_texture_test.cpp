// Textures of the --gpu d3d12 backend (Xenia's D3D12TextureCache) on a synthetic
// PM4 ring: texture fetch constant, untiling and conversion in the loading
// compute shaders, invalidation after a CPU write, with the exact
// value of every texel resolved into guest memory
// (tests/gpu/d3d12_harness.h, rtv path).
//
// Each case: 64x64 texture in physical memory (its own 256 KiB
// block), written by the CPU at the tiled offsets of
// xenia/gpu/texture_address.h (Tiled2D), texture 0 fetch constant
// (SHADER_CONSTANT_FETCH_00..05), point filter, VsPositionTexcoord (vertex
// buffer in vf3: the 6 dwords of texture 0 cover vf0..vf2) + PsTexture2D from
// tests/gpu/ucode_builder.h, full-screen rectangle list (0,0) (64,0)
// (64,64) with texcoord (0,0) (1,0) (1,1): with pix_center kOGLHalf the center
// of the pixel (x+0.5, y+0.5) falls on the center of the texel. Then resolve without clear
// toward a 64x64 tiled destination, endian k8in32, read with the same
// tiled offsets.
//
// Byte order (k_8_8_8_8 with kNone are the bytes R, G, B, A):
//  - the CPU writes the k_8_8_8_8 texel as a big-endian logical DWORD (Put32,
//    like a guest CPU store) with texture endian k8in32: the low
//    byte of the logical DWORD is red, the high one alpha
//    (0xAABBGGRR); identity fetch constant swizzle (RGBA);
//  - the resolve with k8in32 rewrites the same logical DWORD (0xAABBGGRR),
//    read with Get32: so the resolved texel equals the source DWORD.
//    A = 0xFF102030 is (R, G, B, A) = (0x30, 0x20, 0x10, 0xFF).
//  - k_DXT1 with endian k8in16 (like D3D9): every 16-bit word of the
//    block is big-endian in guest memory (color0, color1, low indices,
//    high indices). The expected value is 0xFF<<24 | B8<<16 | G8<<8 | R8, with
//    the 565 -> 888 expansion by replication of the high bits, tolerance +-1.
//
// Cases:
//  - 8x8 checkerboard k_8_8_8_8 (A = 0xFF102030, B = 0xFFC0D0E0);
//  - k_8_8_8_8 with a unique asymmetric value per texel (R = x, G = y,
//    B = x ^ 4y): the checkerboard is constant in every 8x8 square and
//    symmetric in x <-> y, so it does not see permutations of the low bits of
//    x/y, the handling of y & 1, transpositions or shifts of 16 texels
//    in the 32 bpb untiling;
//  - k_DXT1 with one color per 4x4 block (color0 == color1, indices 0);
//  - invalidation: after the first draw the texture's page is observed;
//    the CPU rewrites the checkerboard with A and B swapped (the write goes
//    through the observed pages handler), the second draw gives the swapped
//    checkerboard. Without the invalidation the cache would give the stale texture
//    (end-to-end).
// In all of them: gpu_draws_skipped stays 0.
//
// Every case is a separate TEST_CASE (one backend per process, like ctest).
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include "d3d12_harness.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>

#include "gpu/d3d12_probe.h"
#include "physical_watch.h"
#include "ucode_builder.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/xenos.h"

namespace {

using namespace std::chrono_literals;
using gow2_test::D3D12Harness;
using gow2_test::Pm4Builder;
namespace ub = gow2::ucode_builder;
namespace xenos = xe::gpu::xenos;

constexpr uint32_t kSize = 64;
constexpr uint32_t kBytes = kSize * kSize * 4;
constexpr uint32_t kHalf = kSize / 2;
constexpr uint32_t kSentinel = 0xDEADBEEF;
constexpr uint32_t kCheckerA = 0xFF102030;
constexpr uint32_t kCheckerB = 0xFFC0D0E0;
constexpr uint32_t kVertexFetch = 3;  // vf3 (FETCH_06/07): the first after texture 0
constexpr uint32_t kVertexDwords = 15;  // 3 vertices x (xyz, uv)

uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }

bool HasHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) return false;
  UNSCOPED_INFO("adapter: " << *adapter);
  return true;
}

// Byte offset of block (x, y) of a tiled 2D surface with pitch in
// blocks (aligned to 32) and 2^bpb_log2 bytes per block.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks, unsigned bpb_log2) {
  const uint32_t pitch_aligned = (pitch_blocks + 31) & ~31u;
  return uint32_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), pitch_aligned, bpb_log2));
}

uint32_t Checker(uint32_t x, uint32_t y, bool inverted) {
  const bool b = (((x / 8) + (y / 8)) & 1) != 0;
  return (b != inverted) ? kCheckerB : kCheckerA;
}

// k_8_8_8_8 texel as a big-endian logical DWORD (guest CPU store).
void PutChecker(uint32_t tex, bool inverted) {
  for (uint32_t y = 0; y < kSize; ++y)
    for (uint32_t x = 0; x < kSize; ++x) D3D12Harness::Put32(tex + TiledOffset(x, y, kSize, 2), Checker(x, y, inverted));
}

// Unique asymmetric k_8_8_8_8 texel: (R, G, B, A) = (x, y, x ^ 4y, 0xFF).
uint32_t Unique(uint32_t x, uint32_t y) { return 0xFF000000u | ((x ^ (y << 2)) & 0xFF) << 16 | y << 8 | x; }

void PutUnique(uint32_t tex) {
  for (uint32_t y = 0; y < kSize; ++y)
    for (uint32_t x = 0; x < kSize; ++x) D3D12Harness::Put32(tex + TiledOffset(x, y, kSize, 2), Unique(x, y));
}

// RGB565 color of the DXT1 block (bx, by): different channels between neighboring blocks.
uint16_t BlockColor565(uint32_t bx, uint32_t by) {
  const uint32_t r5 = (bx * 2 + 1) & 31, g6 = (by * 4 + 2) & 63, b5 = ((bx ^ by) * 3 + 5) & 31;
  return uint16_t(r5 << 11 | g6 << 5 | b5);
}

// 565 -> 888 expansion (replication of the high bits) as logical DWORD
// 0xAABBGGRR of the k8in32 resolve.
uint32_t Expand565(uint16_t c) {
  const uint32_t r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
  const uint32_t r = r5 << 3 | r5 >> 2, g = g6 << 2 | g6 >> 4, b = b5 << 3 | b5 >> 2;
  return 0xFF000000u | b << 16 | g << 8 | r;
}

// 8-byte DXT1 blocks, big-endian 16-bit words (endian k8in16):
// color0 == color1, indices 0 (all color0).
void PutDxt1(uint32_t tex) {
  constexpr uint32_t kBlocks = kSize / 4;
  for (uint32_t by = 0; by < kBlocks; ++by)
    for (uint32_t bx = 0; bx < kBlocks; ++bx) {
      const uint32_t at = tex + TiledOffset(bx, by, kBlocks, 3);
      const uint16_t c = BlockColor565(bx, by);
      *kernel::GuestPtr<be<uint16_t>>(at + 0) = c;
      *kernel::GuestPtr<be<uint16_t>>(at + 2) = c;
      *kernel::GuestPtr<be<uint16_t>>(at + 4) = 0;
      *kernel::GuestPtr<be<uint16_t>>(at + 6) = 0;
    }
}

// Fetch constant of a 64x64 tiled 2D texture, point filter, RGBA swizzle, a
// single level, clamp to the edges.
std::array<uint32_t, 6> TextureFetchConstant(uint32_t tex, xenos::TextureFormat format, xenos::Endian endian) {
  xenos::xe_gpu_texture_fetch_t f{};
  f.type = xenos::FetchConstantType::kTexture;
  f.clamp_x = f.clamp_y = f.clamp_z = xenos::ClampMode::kClampToEdge;
  f.pitch = kSize >> 5;
  f.tiled = 1;
  f.format = format;
  f.endianness = endian;
  f.base_address = (tex & 0x1FFFFFFF) >> 12;
  f.size_2d.width = kSize - 1;
  f.size_2d.height = kSize - 1;
  f.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  f.mag_filter = f.min_filter = f.mip_filter = xenos::TextureFilter::kPoint;
  f.aniso_filter = xenos::AnisoFilter::kDisabled;
  f.dimension = xenos::DataDimension::k2DOrStacked;
  return {f.dword_0, f.dword_1, f.dword_2, f.dword_3, f.dword_4, f.dword_5};
}

struct Counters {
  int64_t draws = D3D12Harness::SummaryValue("gpu_draws");
  int64_t skipped = D3D12Harness::SummaryValue("gpu_draws_skipped");
  int64_t resolves = D3D12Harness::SummaryValue("gpu_resolves");
};

class TextureFixture {
 public:
  TextureFixture() : h_("rtv") {
    // The texture has its own 256 KiB block: the readback copy
    // into the resolve destination invalidates and stops observing up to the
    // 64-page block that contains it
    // (SharedMemory::MemoryInvalidationCallback with exact_range = false).
    texture_ = D3D12Harness::AllocPhys(0x40000, 0x40000);
    resolve_vertices_ = D3D12Harness::AllocPhys(0x1000);
    vertices_ = D3D12Harness::AllocPhys(0x1000);
    for (uint32_t& dest : dest_) {
      dest = D3D12Harness::AllocPhys(kBytes);
      REQUIRE(dest);
      for (uint32_t off = 0; off < kBytes; off += 4) D3D12Harness::HostPut32(dest + off, kSentinel);
    }
    REQUIRE((texture_ && resolve_vertices_ && vertices_));
    // (x, y, z, u, v) as big-endian float: (0,0) (64,0) (64,64).
    const float s = float(kSize);
    const uint32_t v[kVertexDwords] = {F(0.f), F(0.f), F(0.f), F(0.f), F(0.f), F(s),   F(0.f), F(0.f),
                                       F(1.f), F(0.f), F(s),   F(s),   F(0.f), F(1.f), F(1.f)};
    for (uint32_t i = 0; i < kVertexDwords; ++i) D3D12Harness::Put32(vertices_ + i * 4, v[i]);
  }

  D3D12Harness& h() { return h_; }
  uint32_t texture() const { return texture_; }

  // Full-screen draw of texture 0 and k8in32 resolve toward dest(i).
  void DrawAndResolve(const std::array<uint32_t, 6>& fetch, int i) {
    const Counters before;
    Pm4Builder p(resolve_vertices_ + resolve_slot_++ * 0x100);
    p.SetSurface(kSize, gow2_test::kMsaa1X, gow2_test::kColorRt8888, 0)
        .ScreenSpaceDrawState()
        .ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionTexcoord(kVertexFetch))
        .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsTexture2D())
        .TextureFetch(0, fetch)
        .VertexFetch(kVertexFetch, vertices_, kVertexDwords)
        .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
    p.ResolveRect(0, 0, kSize, kSize).Resolve(dest_[i], kSize, kSize, gow2_test::kEndian8in32, 0, false);
    REQUIRE(h_.Run(p, 10000ms));
    // 1 draw + 1 resolve (which also counts as a draw), no skips.
    const Counters after;
    CHECK(after.draws == before.draws + 2);
    CHECK(after.resolves == before.resolves + 1);
    CHECK(after.skipped == before.skipped);
    CHECK(after.skipped == 0);
  }

  // Half-resolution draw: 32x32 rectangle with texcoord 0...1 over the 64x64 texture, so every pixel
  // center (x + 0.5) / 32 is exactly on the boundary between texels 2x and 2x + 1; resolve 32x32 toward dest(i).
  void DrawHalfAndResolve(const std::array<uint32_t, 6>& fetch, const std::vector<uint32_t>& ps, int i) {
    const uint32_t half_vertices = vertices_ + 0x800;
    const float s = float(kHalf);
    const uint32_t v[kVertexDwords] = {F(0.f), F(0.f), F(0.f), F(0.f), F(0.f), F(s),   F(0.f), F(0.f),
                                       F(1.f), F(0.f), F(s),   F(s),   F(0.f), F(1.f), F(1.f)};
    for (uint32_t k = 0; k < kVertexDwords; ++k) D3D12Harness::Put32(half_vertices + k * 4, v[k]);
    Pm4Builder p(resolve_vertices_ + resolve_slot_++ * 0x100);
    p.SetSurface(kHalf, gow2_test::kMsaa1X, gow2_test::kColorRt8888, 0)
        .ScreenSpaceDrawState()
        .ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionTexcoord(kVertexFetch))
        .ImLoadImmediate(gow2_test::kShaderPixel, ps)
        .TextureFetch(0, fetch)
        .VertexFetch(kVertexFetch, half_vertices, kVertexDwords)
        .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
    p.ResolveRect(0, 0, kHalf, kHalf).Resolve(dest_[i], kHalf, kHalf, gow2_test::kEndian8in32, 0, false);
    REQUIRE(h_.Run(p, 10000ms));
    CHECK(D3D12Harness::SummaryValue("gpu_draws_skipped") == 0);
  }

  // Compares the kHalf x kHalf resolved texels with expected(x, y).
  void CheckHalfTexels(int i, const std::function<uint32_t(uint32_t, uint32_t)>& expected) {
    uint32_t bad = 0, first_x = 0, first_y = 0, first_got = 0, first_want = 0;
    for (uint32_t y = 0; y < kHalf; ++y)
      for (uint32_t x = 0; x < kHalf; ++x) {
        const uint32_t got = D3D12Harness::Get32(dest_[i] + TiledOffset(x, y, kHalf, 2));
        if (got != expected(x, y) && !bad++) first_x = x, first_y = y, first_got = got, first_want = expected(x, y);
      }
    INFO("dest " << i << ": " << bad << " different texels; the first (" << first_x << "," << first_y << ") = 0x" << std::hex
                 << first_got << ", expected 0x" << first_want);
    CHECK(bad == 0);
  }

  // Compares the 4096 resolved texels (tiled offsets of the destination, big-endian
  // logical DWORD) with expected(x, y), channel by channel within tolerance.
  void CheckTexels(int i, const std::function<uint32_t(uint32_t, uint32_t)>& expected, int tolerance = 0) {
    uint32_t bad = 0, first_x = 0, first_y = 0, first_got = 0, first_want = 0;
    for (uint32_t y = 0; y < kSize; ++y)
      for (uint32_t x = 0; x < kSize; ++x) {
        const uint32_t got = D3D12Harness::Get32(dest_[i] + TiledOffset(x, y, kSize, 2));
        const uint32_t want = expected(x, y);
        bool ok = true;
        for (int c = 0; c < 32; c += 8)
          ok &= std::abs(int((got >> c) & 0xFF) - int((want >> c) & 0xFF)) <= tolerance;
        if (!ok && !bad++) {
          first_x = x, first_y = y, first_got = got, first_want = want;
        }
      }
    INFO("dest " << i << ": " << bad << " different texels; the first (" << first_x << "," << first_y << ") = 0x" << std::hex
                 << first_got << ", expected 0x" << first_want);
    CHECK(bad == 0);
  }

  void CheckLogClean() {
    const std::string log = h_.StopAndReadLog();
    INFO(log);
    CHECK(log.find("device removed") == std::string::npos);
    CHECK(log.find("[gpu] fatal") == std::string::npos);
    CHECK(log.find("Failed in backend") == std::string::npos);
  }

 private:
  D3D12Harness h_;
  uint32_t texture_ = 0, resolve_vertices_ = 0, vertices_ = 0;
  uint32_t dest_[2] = {};
  uint32_t resolve_slot_ = 0;
};

}  // namespace

TEST_CASE("d3d12_texture: k_8_8_8_8 tiled 8x8 checkerboard", "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  PutChecker(f.texture(), false);
  f.DrawAndResolve(TextureFetchConstant(f.texture(), xenos::TextureFormat::k_8_8_8_8, xenos::Endian::k8in32), 0);
  f.CheckTexels(0, [](uint32_t x, uint32_t y) { return Checker(x, y, false); });
  f.CheckLogClean();
}

TEST_CASE("d3d12_texture: k_8_8_8_8 tiled with a unique value per texel", "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  PutUnique(f.texture());
  f.DrawAndResolve(TextureFetchConstant(f.texture(), xenos::TextureFormat::k_8_8_8_8, xenos::Endian::k8in32), 0);
  f.CheckTexels(0, Unique);
  f.CheckLogClean();
}

TEST_CASE("d3d12_texture: k_DXT1 tiled, one color per 4x4 block", "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  PutDxt1(f.texture());
  f.DrawAndResolve(TextureFetchConstant(f.texture(), xenos::TextureFormat::k_DXT1, xenos::Endian::k8in16), 0);
  f.CheckTexels(0, [](uint32_t x, uint32_t y) { return Expand565(BlockColor565(x / 4, y / 4)); }, 1);
  f.CheckLogClean();
}

TEST_CASE("d3d12_texture: the CPU rewrites the texture, the next draw is not stale", "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  const auto fetch = TextureFetchConstant(f.texture(), xenos::TextureFormat::k_8_8_8_8, xenos::Endian::k8in32);
  PutChecker(f.texture(), false);
  f.DrawAndResolve(fetch, 0);
  f.CheckTexels(0, [](uint32_t x, uint32_t y) { return Checker(x, y, false); });
  // After loading, Xenia's shared memory observes the texture's pages
  // (all 4: 16 KiB).
  for (uint32_t off = 0; off < kBytes; off += 0x1000) REQUIRE(physwatch::IsWatched((f.texture() + off) & 0x1FFFFFFF));

  // The CPU swaps A and B: every page goes through the observed pages
  // handler (invalidation of the shared memory and the texture) and reopens.
  PutChecker(f.texture(), true);
  for (uint32_t off = 0; off < kBytes; off += 0x1000)
    CHECK_FALSE(physwatch::IsWatched((f.texture() + off) & 0x1FFFFFFF));
  CHECK(D3D12Harness::Get32(f.texture() + TiledOffset(0, 0, kSize, 2)) == kCheckerB);

  f.DrawAndResolve(fetch, 1);
  f.CheckTexels(1, [](uint32_t x, uint32_t y) { return Checker(x, y, true); });
  f.CheckLogClean();
}

// GoW3 builds its half-resolution occlusion query depth with a depth-writing pass whose point-sampled 2x2 fetches
// land exactly on texel boundaries; the console takes the upper-left texel there (the 2x2 footprint the half-res
// pixel covers, so the min is conservative). With the right/lower one the floors' query boxes fail by a fraction
// of a pixel against the floors themselves and the floors flash.
TEST_CASE("d3d12_texture: a depth-writing pass point-samples the upper-left texel on exact texel boundaries",
          "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  PutUnique(f.texture());
  f.DrawHalfAndResolve(TextureFetchConstant(f.texture(), xenos::TextureFormat::k_8_8_8_8, xenos::Endian::k8in32),
                       ub::PsTexture2DWritingDepth(), 0);
  f.CheckHalfTexels(0, [](uint32_t x, uint32_t y) { return Unique(2 * x, 2 * y); });
  f.CheckLogClean();
}

// Color passes keep Xenia's right/lower texel on exact boundaries (kTextureCoordEpsilon).
TEST_CASE("d3d12_texture: a color pass point-samples the lower-right texel on exact texel boundaries",
          "[d3d12_texture]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  TextureFixture f;
  PutUnique(f.texture());
  f.DrawHalfAndResolve(TextureFetchConstant(f.texture(), xenos::TextureFormat::k_8_8_8_8, xenos::Endian::k8in32),
                       ub::PsTexture2D(), 0);
  f.CheckHalfTexels(0, [](uint32_t x, uint32_t y) { return Unique(2 * x + 1, 2 * y + 1); });
  f.CheckLogClean();
}
