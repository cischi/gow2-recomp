// EDRAM of the --gpu d3d12 backend (Xenia's D3D12CommandProcessor): clear and
// resolve on a synthetic PM4 ring, with the exact values of the DWORDs written to
// guest memory (readback_resolve=full, tests/gpu/d3d12_harness.h).
//
// 64x64 k_8_8_8_8 surface, MSAA 1x, EDRAM base 0:
//  1. resolve with clear to C = 0xFF336699 toward A (A is not checked: it copies
//     the EDRAM before the clear);
//  2. resolve without clear toward B, copy_dest_endian kNone: every host DWORD
//     of the 64x64 equals C;
//  3. resolve without clear toward B', k8in32: every DWORD equals byteswap(C).
// The summary counts 3 resolves and no skipped draw. "rtv" path (the game's)
// and "rov", skipped if the device does not support ROVs.
//
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include "d3d12_harness.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu/d3d12_probe.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace {

using namespace std::chrono_literals;
using gow2_test::D3D12Harness;
using gow2_test::Pm4Builder;

constexpr uint32_t kClearColor = 0xFF336699;
constexpr uint32_t kSize = 64;
constexpr uint32_t kBytes = kSize * kSize * 4;  // 64x64 tiled 32 bpp = 4 macro-tiles 32x32
constexpr uint32_t kSentinel = 0xDEADBEEF;
constexpr uint32_t kGuardBytes = 0x1000;  // after the resolve extent: not touched

bool HasHardwareAdapter() {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) return false;
  UNSCOPED_INFO("adapter: " << *adapter);
  return true;
}

// Host DWORDs in [addr, addr + bytes) different from expected.
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

void ClearAndResolve(std::string_view render_target_path) {
  D3D12Harness h(render_target_path);

  const uint32_t vertices = D3D12Harness::AllocPhys(0x1000);
  const uint32_t a = D3D12Harness::AllocPhys(kBytes + kGuardBytes);
  const uint32_t b = D3D12Harness::AllocPhys(kBytes + kGuardBytes);
  const uint32_t b_swapped = D3D12Harness::AllocPhys(kBytes + kGuardBytes);
  REQUIRE((vertices && a && b && b_swapped));
  for (uint32_t dest : {a, b, b_swapped})
    for (uint32_t off = 0; off < kBytes + kGuardBytes; off += 4) D3D12Harness::HostPut32(dest + off, kSentinel);

  const int64_t resolves = D3D12Harness::SummaryValue("gpu_resolves");
  const int64_t skipped = D3D12Harness::SummaryValue("gpu_draws_skipped");
  REQUIRE(resolves >= 0);
  REQUIRE(skipped >= 0);

  Pm4Builder p(vertices);
  p.SetSurface(kSize, gow2_test::kMsaa1X, gow2_test::kColorRt8888, 0)
      .ResolveRect(0, 0, kSize, kSize)
      .Resolve(a, kSize, kSize, gow2_test::kEndianNone, kClearColor, true)
      .Resolve(b, kSize, kSize, gow2_test::kEndianNone, 0, false)
      .Resolve(b_swapped, kSize, kSize, gow2_test::kEndian8in32, 0, false);
  CHECK(h.Run(p, 5000ms));

  uint32_t first_bad = 0;
  INFO("path " << render_target_path);
  {
    const uint32_t bad = CountDifferent(b, kBytes, kClearColor, first_bad);
    INFO("B: " << bad << " different DWORDs, the first 0x" << std::hex << first_bad);
    CHECK(bad == 0);
  }
  {
    const uint32_t bad = CountDifferent(b_swapped, kBytes, std::byteswap(kClearColor), first_bad);
    INFO("B': " << bad << " different DWORDs, the first 0x" << std::hex << first_bad);
    CHECK(bad == 0);
  }
  // The resolve writes only the 64x64 (tiled extent = 16 KiB).
  CHECK(CountDifferent(b + kBytes, kGuardBytes, kSentinel, first_bad) == 0);
  CHECK(CountDifferent(b_swapped + kBytes, kGuardBytes, kSentinel, first_bad) == 0);
  CHECK(CountDifferent(a + kBytes, kGuardBytes, kSentinel, first_bad) == 0);

  CHECK(D3D12Harness::SummaryValue("gpu_resolves") == resolves + 3);
  CHECK(D3D12Harness::SummaryValue("gpu_draws_skipped") == skipped);

  const std::string log = h.StopAndReadLog();
  INFO(log);
  CHECK(log.find("device removed") == std::string::npos);
  CHECK(log.find("[gpu] fatal") == std::string::npos);
}

}  // namespace

TEST_CASE("d3d12_edram: clear and resolve 64x64 k_8_8_8_8 on the rtv path, kNone and k8in32", "[d3d12_edram]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  ClearAndResolve("rtv");
}

TEST_CASE("d3d12_edram: clear and resolve 64x64 k_8_8_8_8 on the rov path, kNone and k8in32", "[d3d12_edram]") {
  if (!HasHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  {
    // Without ROV D3D12RenderTargetCache::Initialize falls back to rtv.
    auto provider = xe::ui::d3d12::D3D12Provider::Create();
    REQUIRE(provider);
    if (!provider->AreRasterizerOrderedViewsSupported()) SKIP("the device does not support ROVs");
  }
  ClearAndResolve("rov");
}
