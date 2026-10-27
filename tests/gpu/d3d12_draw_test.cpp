// Draws of the --gpu d3d12 backend (Xenia's D3D12CommandProcessor) with Xenos
// shaders translated to DXBC from the microcode, on a synthetic PM4 ring: the most
// frequent packets of the game (IM_LOAD / IM_LOAD_IMMEDIATE, SET_CONSTANT /
// LOAD_ALU_CONSTANT, DRAW_INDX / DRAW_INDX_2) with the exact value of each
// texel resolved into guest memory (tests/gpu/d3d12_harness.h, rtv path).
//
// Each case: 64x64 k_8_8_8_8 surface at EDRAM base 0, VS
// VsPositionFromStream (vf0, big-endian float xyz, k8in32) and PS
// PsConstantColor (oC0 = c0) from tests/gpu/ucode_builder.h, rectangle list
// (0,0) (64,0) (64,64) in pixels (PA_CL_VTE_CNTL without viewport
// scale/offset, clip disabled), then resolve without clear toward a
// destination full of 0xDEADBEEF, endian kNone: all 4096 host DWORDs
// equal the PS color.
//
//  - IM_LOAD_IMMEDIATE + SET_CONSTANT c0 = (0,1,0,1) + DRAW_INDX_2 auto-index;
//  - IM_LOAD from physical memory + LOAD_ALU_CONSTANT;
//  - DRAW_INDX with 3 big-endian 16-bit indices (k8in16) in physical memory,
//    which skip a decoy vertex at the head of the vertex buffer;
//  - draw without a vertex shader: gpu_draws_skipped +1 (IssueDraw fails),
//    the command processor continues;
//  - stale shader: the PS microcode of an IM_LOAD sits in the vertex
//    buffer's page, observed after the first draw; rewritten by the CPU to
//    read c1 = (1,0,0,1), the next draw (new IM_LOAD) is red.
//
// Every case is a separate TEST_CASE (one backend per process, like ctest).
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include "d3d12_harness.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "physical_watch.h"
#include "ucode_builder.h"

namespace {

using namespace std::chrono_literals;
using gow2_test::D3D12Harness;
using gow2_test::Pm4Builder;
namespace ub = gow2::ucode_builder;

constexpr uint32_t kSize = 64;
constexpr uint32_t kBytes = kSize * kSize * 4;
constexpr uint32_t kSentinel = 0xDEADBEEF;
constexpr uint32_t kGreen = 0xFF00FF00;  // (0,1,0,1) in k_8_8_8_8, kNone
// (1,0,0,1): bytes R, G, B, A in memory with kNone (Xenos k_8_8_8_8,
// red in the low byte); with copy_dest_swap (D3D9's ARGB destination)
// it is the D3DCOLOR 0xFFFF0000.
constexpr uint32_t kRedRgba = 0xFF0000FF;
constexpr uint32_t kRedArgb = 0xFFFF0000;

uint32_t F(float f) { return std::bit_cast<uint32_t>(f); }

bool HasHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) return false;
  UNSCOPED_INFO("adapter: " << *adapter);
  return true;
}

uint32_t CountDifferent(uint32_t addr, uint32_t bytes, uint32_t expected, uint32_t& first_bad) {
  uint32_t bad = 0;
  for (uint32_t off = 0; off < bytes; off += 4) {
    if (D3D12Harness::HostGet32(addr + off) != expected) {
      if (!bad) first_bad = D3D12Harness::HostGet32(addr + off);
      ++bad;
    }
  }
  return bad;
}

// Logical dwords in big-endian guest memory (as the guest CPU writes them).
void PutWords(uint32_t addr, const std::vector<uint32_t>& words) {
  for (size_t i = 0; i < words.size(); ++i) D3D12Harness::Put32(addr + uint32_t(i) * 4, words[i]);
}

// Big-endian float xyz vertices; the first is a decoy outside the rectangle
// (only the indexed draw skips it), then (0,0) (64,0) (64,64).
constexpr uint32_t kDecoyBytes = 12;
void PutVertices(uint32_t addr) {
  const float s = float(kSize);
  PutWords(addr, {F(-100.f), F(-100.f), F(0.f), F(0.f), F(0.f), F(0.f), F(s), F(0.f), F(0.f), F(s), F(s), F(0.f)});
}

struct Counters {
  int64_t draws = D3D12Harness::SummaryValue("gpu_draws");
  int64_t skipped = D3D12Harness::SummaryValue("gpu_draws_skipped");
  int64_t resolves = D3D12Harness::SummaryValue("gpu_resolves");
};

class DrawFixture {
 public:
  DrawFixture() : h_("rtv") {
    resolve_vertices_ = D3D12Harness::AllocPhys(0x1000);
    // The vertex buffer has its own 256 KiB block: Xenia's shared memory,
    // when the CPU writes to an observed page (also the readback copy
    // into the resolve destination), invalidates and stops
    // observing up to the 64-page block that contains it
    // (SharedMemory::MemoryInvalidationCallback with exact_range = false).
    vertices_ = D3D12Harness::AllocPhys(0x40000, 0x40000);
    data_ = D3D12Harness::AllocPhys(0x1000);
    for (uint32_t& dest : dest_) {
      dest = D3D12Harness::AllocPhys(kBytes);
      REQUIRE(dest);
    }
    REQUIRE((resolve_vertices_ && vertices_ && data_));
    PutVertices(vertices_);
    for (uint32_t dest : dest_)
      for (uint32_t off = 0; off < kBytes; off += 4) D3D12Harness::HostPut32(dest + off, kSentinel);
  }

  D3D12Harness& h() { return h_; }
  uint32_t vertices() const { return vertices_; }  // vertex buffer page
  uint32_t data() const { return data_; }          // free page for shaders, constants, indices
  uint32_t dest(int i) const { return dest_[i]; }

  // Surface and draw state; the caller adds shaders, constants and draw.
  Pm4Builder Begin() {
    Pm4Builder p(resolve_vertices_ + resolve_slot_ * 0x100);
    p.SetSurface(kSize, gow2_test::kMsaa1X, gow2_test::kColorRt8888, 0).ScreenSpaceDrawState();
    return p;
  }

  // Resolve without clear of the render target toward dest(i), kNone (and, if
  // swapped >= 0, a second one toward dest(swapped) with copy_dest_swap);
  // runs the ring.
  void ResolveAndRun(Pm4Builder& p, int i, int swapped = -1) {
    ++resolve_slot_;
    p.ResolveRect(0, 0, kSize, kSize).Resolve(dest_[i], kSize, kSize, gow2_test::kEndianNone, 0, false);
    if (swapped >= 0)
      p.Resolve(dest_[swapped], kSize, kSize, gow2_test::kEndianNone, 0, false, gow2_test::kColorFormat8888, true);
    REQUIRE(h_.Run(p, 10000ms));
  }

  void CheckColor(int i, uint32_t expected) {
    uint32_t first_bad = 0;
    const uint32_t bad = CountDifferent(dest_[i], kBytes, expected, first_bad);
    INFO("dest " << i << ": " << bad << " DWORDs different from 0x" << std::hex << expected << ", the first 0x" << first_bad);
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
  uint32_t resolve_vertices_ = 0, vertices_ = 0, data_ = 0;
  uint32_t dest_[4] = {};
  uint32_t resolve_slot_ = 0;
};

// Successful draw (1 draw + the resolves, which also count as draws), no skips.
void CheckOneDrawOneResolve(const Counters& before, int resolves = 1) {
  const Counters after;
  CHECK(after.draws == before.draws + 1 + resolves);
  CHECK(after.resolves == before.resolves + resolves);
  CHECK(after.skipped == before.skipped);
}

constexpr uint32_t kVertexDwords = 9;  // 3 xyz vertices

}  // namespace

TEST_CASE("d3d12_draw: IM_LOAD_IMMEDIATE triangle, SET_CONSTANT and DRAW_INDX_2 auto-index", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DrawFixture f;
  const Counters before;
  Pm4Builder p = f.Begin();
  p.ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  f.ResolveAndRun(p, 0);
  f.CheckColor(0, kGreen);
  CheckOneDrawOneResolve(before);
  f.CheckLogClean();
}

// d3d12_submit_every_draws=1 (patch 0016): a submission closed after every
// draw; the second draw covers the first in the next submission and the
// resolve, in yet another, sees the second color.
TEST_CASE("d3d12_draw: one submission per draw, same result", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  REQUIRE(gpu::SetXeniaCvar("d3d12_submit_every_draws", "1"));
  DrawFixture f;
  const Counters before;
  Pm4Builder p = f.Begin();
  p.ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3)
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(1.f), F(0.f), F(0.f), F(1.f)})
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  f.ResolveAndRun(p, 0);
  f.CheckColor(0, kRedRgba);
  const Counters after;
  CHECK(after.draws == before.draws + 3);
  CHECK(after.skipped == before.skipped);
  f.CheckLogClean();
}

// ZPD report (strict occlusion query) around two overlapping 64x64 draws
// with d3d12_submit_every_draws=1 (patch 0016, active only at scale 1): the
// report is split between submissions and the difference between the end report and
// the begin one remains the samples of the two draws.
TEST_CASE("d3d12_draw: ZPD report with one submission per draw", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  REQUIRE(gpu::SetXeniaCvar("occlusion_query", "strict"));
  REQUIRE(gpu::SetXeniaCvar("d3d12_submit_every_draws", "1"));
  DrawFixture f;
  constexpr uint32_t kRbSampleCountAddr = 0x2325;
  constexpr uint32_t kZpassDone = 0x15;
  constexpr uint32_t kPending = 0xFFFFFEED;  // D3D sentinel, big-endian in memory
  const uint32_t begin = f.data() + 0x800, end = begin + 32;
  for (uint32_t r : {begin, end})
    for (uint32_t off = 0; off < 32; off += 4) D3D12Harness::Put32(r + off, kPending);
  Pm4Builder p = f.Begin();
  p.ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .SetRegs(kRbSampleCountAddr, {begin})
      .Type3(gpu::kPm4EventWriteZpd, {kZpassDone})
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3)
      .SetRegs(kRbSampleCountAddr, {end})
      .Type3(gpu::kPm4EventWriteZpd, {kZpassDone});
  f.ResolveAndRun(p, 0);
  // ZPass = A + B (little-endian), written when the ring is empty.
  const auto zpass = [](uint32_t r) { return D3D12Harness::HostGet32(r + 16) + D3D12Harness::HostGet32(r + 20); };
  REQUIRE(D3D12Harness::Eventually([&] { return D3D12Harness::Get32(end + 16) != kPending; }, 5000ms));
  INFO("begin " << zpass(begin) << " end " << zpass(end));
  CHECK(zpass(end) - zpass(begin) == 2 * kSize * kSize);
  f.CheckLogClean();
}

TEST_CASE("d3d12_draw: IM_LOAD from physical memory and LOAD_ALU_CONSTANT", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DrawFixture f;
  const auto vs = ub::VsPositionFromStream();
  const auto ps = ub::PsConstantColor();
  const uint32_t vs_at = f.data(), ps_at = f.data() + 0x100, c_at = f.data() + 0x200;
  PutWords(vs_at, vs);
  PutWords(ps_at, ps);
  PutWords(c_at, {F(0.f), F(1.f), F(0.f), F(1.f)});
  const Counters before;
  Pm4Builder p = f.Begin();
  p.ImLoad(gow2_test::kShaderVertex, vs_at, uint32_t(vs.size()))
      .ImLoad(gow2_test::kShaderPixel, ps_at, uint32_t(ps.size()))
      .LoadAluConstants(c_at, gow2_test::kAluConstantPs0, 4)
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  f.ResolveAndRun(p, 0);
  f.CheckColor(0, kGreen);
  CheckOneDrawOneResolve(before);
  f.CheckLogClean();
}

TEST_CASE("d3d12_draw: DRAW_INDX with 16-bit big-endian indices (k8in16)", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DrawFixture f;
  // Indices 1, 2, 3 (vertex 0 is the decoy) in 16-bit big-endian.
  const uint32_t indices = f.data();
  *kernel::GuestPtr<be<uint16_t>>(indices + 0) = 1;
  *kernel::GuestPtr<be<uint16_t>>(indices + 2) = 2;
  *kernel::GuestPtr<be<uint16_t>>(indices + 4) = 3;
  const Counters before;
  Pm4Builder p = f.Begin();
  p.ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(f.vertices(), kVertexDwords + 3)
      .DrawIndx16(gow2_test::kPrimRectangleList, 3, indices, gow2_test::kEndian8in16);
  f.ResolveAndRun(p, 0);
  f.CheckColor(0, kGreen);
  CheckOneDrawOneResolve(before);
  f.CheckLogClean();
}

TEST_CASE("d3d12_draw: draw without a vertex shader counts in gpu_draws_skipped", "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DrawFixture f;
  const Counters before;
  // No IM_LOAD in this process: active_vertex_shader() is null.
  Pm4Builder p = f.Begin();
  p.SetAluConstants(gow2_test::kAluConstantPs0, {F(0.f), F(1.f), F(0.f), F(1.f)})
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  REQUIRE(f.h().Run(p, 10000ms));
  {
    const Counters after;
    CHECK(after.draws == before.draws + 1);
    CHECK(after.skipped == before.skipped + 1);
  }
  // The command processor continues: a complete draw afterwards succeeds.
  const Counters before_ok;
  Pm4Builder q = f.Begin();
  q.ImLoadImmediate(gow2_test::kShaderVertex, ub::VsPositionFromStream())
      .ImLoadImmediate(gow2_test::kShaderPixel, ub::PsConstantColor())
      .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
      .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
  f.ResolveAndRun(q, 0);
  f.CheckColor(0, kGreen);
  CheckOneDrawOneResolve(before_ok);
  const std::string log = f.h().StopAndReadLog();
  INFO(log);
  CHECK(log.find("device removed") == std::string::npos);
  CHECK(log.find("[gpu] fatal") == std::string::npos);
}

TEST_CASE("d3d12_draw: IM_LOAD microcode rewritten in an observed page, no stale shader",
          "[d3d12_draw]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  DrawFixture f;
  const auto vs = ub::VsPositionFromStream();
  const auto green = ub::PsConstantColor(0);
  const auto red = ub::PsConstantColor(1);
  REQUIRE(green.size() == red.size());
  REQUIRE(green != red);
  // VS in a separate page; PS in the vertex buffer's page (observed
  // by Xenia's shared memory after the first draw).
  const uint32_t vs_at = f.data();
  const uint32_t ps_at = f.vertices() + 0x800;
  PutWords(vs_at, vs);
  PutWords(ps_at, green);
  auto draw = [&](int dest, int swapped) {
    const Counters before;
    Pm4Builder p = f.Begin();
    p.ImLoad(gow2_test::kShaderVertex, vs_at, uint32_t(vs.size()))
        .ImLoad(gow2_test::kShaderPixel, ps_at, uint32_t(green.size()))
        .SetAluConstants(gow2_test::kAluConstantPs0,
                         {F(0.f), F(1.f), F(0.f), F(1.f), F(1.f), F(0.f), F(0.f), F(1.f)})
        .VertexFetch0(f.vertices() + kDecoyBytes, kVertexDwords)
        .DrawIndx2Auto(gow2_test::kPrimRectangleList, 3);
    f.ResolveAndRun(p, dest, swapped);
    CheckOneDrawOneResolve(before, 2);
  };

  draw(0, 2);
  f.CheckColor(0, kGreen);
  f.CheckColor(2, kGreen);  // (0,1,0,1) is the same with red and blue swapped
  REQUIRE(physwatch::IsWatched(ps_at & 0x1FFFFFFF));

  // The CPU rewrites the microcode: the write goes through the observed
  // pages handler (invalidation of the shared memory) and the page reopens.
  PutWords(ps_at, red);
  CHECK_FALSE(physwatch::IsWatched(ps_at & 0x1FFFFFFF));
  for (size_t i = 0; i < red.size(); ++i) REQUIRE(D3D12Harness::Get32(ps_at + uint32_t(i) * 4) == red[i]);

  draw(1, 3);
  f.CheckColor(1, kRedRgba);
  f.CheckColor(3, kRedArgb);
  f.CheckLogClean();
}
