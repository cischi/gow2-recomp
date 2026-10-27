// HDR passes on the GPU (runtime/gpu/hdr_passes.h): residual reconstruction and PQ composition compared with
// runtime/hdr_math. SKIP without a hardware D3D12 adapter.
#include <catch2/catch_test_macros.hpp>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "gpu/hdr_passes.h"
#include "gpu_texture_util.h"
#include "hdr_math.h"

namespace {

using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
using gow2_test::ComPtr;
using gow2_test::MakeTexture;

constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kW = 8, kH = 8;

// Constants of the simple path: no motion blur or DoF, all taps fall on the pixel and pre = tf1.
gpu::HdrUberConstants SimpleConstants() {
  gpu::HdrUberConstants k{};
  auto set = [](float* d, float x, float y, float z, float w) {
    d[0] = x;
    d[1] = y;
    d[2] = z;
    d[3] = w;
  };
  set(k.c[0], 1, 0, 0, 0);
  set(k.c[2], 0, 0, 0, -1);  // linear depth 1
  set(k.c[3], 300, 1e30f, 4, 0);
  set(k.c[5], 0, 0, 0, 1);  // c5..c10: second case of the uber post tonemapping test (hdr_math_test.cpp)
  set(k.c[6], 1, 1, 1, 0);
  set(k.c[7], 1, 1, 1, 0);
  set(k.c[9], 1, 1, 1, 0.4545f);
  set(k.c[11], 0, 0, 1, 1);
  set(k.c[12], 24, -2, 0, 0);
  set(k.c[13], 1, 0, 0, 0);
  set(k.c[14], 0, 1, 0, 0);
  set(k.c[15], 0, 0, 0, 1);
  set(k.c[17], 1, 1, 1, 1);  // do not divide by 0
  set(k.c253, 4, 65504, 0, 0);
  set(k.c254, 2, 14, 1e-4f, 1);
  set(k.c255, 0.2f, 0.4f, 0.001f, -1);
  for (int i = 0; i < 3; ++i) k.exp_scale[i] = 1;
  return k;
}

hdr::UberTonemapConstants Tonemap(const gpu::HdrUberConstants& k) {
  hdr::UberTonemapConstants t{};
  std::memcpy(t.c5, k.c[5], 16);
  std::memcpy(t.c6, k.c[6], 16);
  std::memcpy(t.c7, k.c[7], 16);
  std::memcpy(t.c8, k.c[8], 16);
  std::memcpy(t.c9, k.c[9], 16);
  std::memcpy(t.c10, k.c[10], 16);
  t.c254z = k.c254[2];
  return t;
}

gpu::GammaRamp IdentityRamp() {
  gpu::GammaRamp r{};
  r.pwl = false;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = uint32_t(std::lround(i * 1023.0 / 255.0));
    r.table[i] = v << 20 | v << 10 | v;
  }
  return r;
}

// Pow of the ucode, like hdr_math.
float UPow(float x, float e) { return std::exp2(std::log2(std::max(std::fabs(x), 1e-4f)) * e); }

uint32_t Gray10(float v) {
  const uint32_t c = uint32_t(std::lround(v * 1023.0f));
  return c | c << 10 | c << 20 | 3u << 30;
}

uint16_t Half(float v) { return XMConvertFloatToHalf(v); }

bool Near(float a, float b, float rel, float abs) { return std::fabs(a - b) <= abs + rel * std::fabs(b); }

// `texels` copies of the same texel (compact data for Upload).
template <typename T>
std::vector<T> Fill(size_t texels, std::initializer_list<T> texel) {
  std::vector<T> v;
  v.reserve(texels * texel.size());
  for (size_t i = 0; i < texels; ++i) v.insert(v.end(), texel);
  return v;
}

const hdr::Rgb kPre[4] = {{0.5f, 0.5f, 0.5f}, {1, 1, 1}, {2, 0.5f, 0.25f}, {4, 4, 4}};

}  // namespace

namespace {

// Non-identity PWL (half of the identity: base = seg * 8 * 32, slope 8 * 32), different from the identity table:
// a shader that ignored the PWL would give other codes. Reference: same formula as the shader.
void MakePwl(gpu::GammaRamp& r) {
  r.pwl = true;
  for (uint32_t seg = 0; seg < 128; ++seg)
    for (uint32_t c = 0; c < 3; ++c) r.pwl_rgb[seg * 3 + c] = (8u * 32u) << 16 | (seg * 8u * 32u);
}

uint32_t PwlCode10(uint32_t i) {
  const uint32_t w = (8u * 32u) << 16 | ((i >> 3) * 8u * 32u);
  const float v = (float(w & 0xFFFF) + float((i & 7) * (w >> 16)) * (1.0f / 8.0f)) * (1.0f / (64.0f * 1023.0f));
  return uint32_t(std::lround(std::min(v, 1.0f) * 1023.0f));
}

void CheckReconstruct(const xe::ui::d3d12::D3D12Provider& pr, bool pwl) {
  auto provider = &pr;
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::HdrPasses::Create(d);
  REQUIRE(passes);

  const gpu::HdrUberConstants k = SimpleConstants();
  const hdr::UberTonemapConstants t = Tonemap(k);
  gpu::GammaRamp ramp = IdentityRamp();
  if (pwl) MakePwl(ramp);
  const float c9w = k.c[9][3], peak_over_paper = 5;

  // Per column: pre, y, yh, 8-bit prehud and its 10-bit ramp.
  std::vector<uint16_t> scene(kW * kH * 4);
  std::vector<uint8_t> prehud(kW * kH * 4);
  std::vector<uint32_t> guest(kW * kH);
  hdr::UberTonemapOut tm[kW];
  hdr::Rgb ramp_prehud[kW];
  for (uint32_t x = 0; x < kW; ++x) {
    const hdr::Rgb pre = kPre[x % 4];
    tm[x] = hdr::UberTonemap(pre, t);
    const float y[3] = {tm[x].y.r, tm[x].y.g, tm[x].y.b};
    float rp[3];
    uint32_t g10 = 3u << 30;
    for (int c = 0; c < 3; ++c) {
      const uint32_t code8 = uint32_t(std::lround(255.0f * UPow(y[c], c9w)));
      const uint32_t code10 = pwl ? PwlCode10(uint32_t(code8 / 255.0f * 1023.0f + 0.5f)) : ramp.table[code8] & 1023;
      rp[c] = code10 / 1023.0f;
      g10 |= code10 << (10 * c);
      for (uint32_t row = 0; row < kH; ++row) prehud[(row * kW + x) * 4 + c] = uint8_t(code8);
    }
    ramp_prehud[x] = {rp[0], rp[1], rp[2]};
    const float pv[3] = {pre.r, pre.g, pre.b};
    for (uint32_t row = 0; row < kH; ++row) {
      for (int c = 0; c < 3; ++c) scene[(row * kW + x) * 4 + c] = Half(pv[c]);
      scene[(row * kW + x) * 4 + 3] = Half(0);
      prehud[(row * kW + x) * 4 + 3] = 255;
      guest[row * kW + x] = row >= 6 ? 0xFFFFFFFFu : g10;  // rows 6-7: white HUD
    }
  }

  auto depth = MakeTexture(d, kW, kH, DXGI_FORMAT_R32_TYPELESS, kRead);
  auto scene_tex = MakeTexture(d, kW, kH, DXGI_FORMAT_R16G16B16A16_TYPELESS, kRead);
  auto bloom = MakeTexture(d, 4, 4, DXGI_FORMAT_R16G16B16A16_FLOAT, kRead);
  auto velocity = MakeTexture(d, kW, kH, DXGI_FORMAT_R16G16_TYPELESS, kRead);
  auto prehud_tex = MakeTexture(d, kW, kH, DXGI_FORMAT_R8G8B8A8_TYPELESS, kRead);
  auto guest_tex = MakeTexture(d, kW, kH, DXGI_FORMAT_R10G10B10A2_TYPELESS, kRead);
  auto residual = MakeTexture(d, kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const auto depth_data = Fill<float>(kW * kH, {0.5f});
  const auto zeros = std::vector<uint16_t>(kW * kH * 4, 0);
  gow2_test::Upload(*provider, depth.Get(), 4, depth_data.data(), kRead);
  gow2_test::Upload(*provider, scene_tex.Get(), 8, scene.data(), kRead);
  gow2_test::Upload(*provider, bloom.Get(), 8, zeros.data(), kRead);
  gow2_test::Upload(*provider, velocity.Get(), 4, zeros.data(), kRead);
  gow2_test::Upload(*provider, prehud_tex.Get(), 4, prehud.data(), kRead);
  gow2_test::Upload(*provider, guest_tex.Get(), 4, guest.data(), kRead);
  gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
    passes->Reconstruct(l, depth.Get(), scene_tex.Get(), bloom.Get(), velocity.Get(), prehud_tex.Get(),
                        guest_tex.Get(), k, ramp, peak_over_paper, residual.Get(), 1);
  });
  const auto raw = gow2_test::Readback(*provider, residual.Get(), 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  for (uint32_t row = 0; row < kH; ++row)
    for (uint32_t x = 0; x < kW; ++x) {
      INFO("pixel " << x << "," << row);
      uint16_t h[4];
      std::memcpy(h, raw.data() + (row * kW + x) * 8, 8);
      const hdr::Rgb rp = ramp_prehud[x];
      const hdr::Rgb g = row >= 6 ? hdr::Rgb{1, 1, 1} : rp;
      const float m = hdr::HudMask(g, rp);
      const hdr::Rgb l_sdr = {hdr::SrgbToLinear(rp.r), hdr::SrgbToLinear(rp.g), hdr::SrgbToLinear(rp.b)};
      const hdr::Rgb e = hdr::Residual(l_sdr, tm[x].y, tm[x].y_unclamped, c9w, peak_over_paper, m);
      CHECK(Near(XMConvertHalfToFloat(h[0]), e.r, 2e-3f, 1e-3f));
      CHECK(Near(XMConvertHalfToFloat(h[1]), e.g, 2e-3f, 1e-3f));
      CHECK(Near(XMConvertHalfToFloat(h[2]), e.b, 2e-3f, 1e-3f));
      CHECK(Near(XMConvertHalfToFloat(h[3]), m, 2e-3f, 1e-3f));
      // No residual where the HUD covers (full mask) nor where the scene does not saturate (pre 0.5).
      if (m == 1.0f || x % 4 == 0)
        for (int c = 0; c < 3; ++c) CHECK(XMConvertHalfToFloat(h[c]) == 0.0f);
    }
  // The case really measures something: positive residual in column 4 above the HUD, full mask in column 0.
  CHECK(hdr::HudMask({1, 1, 1}, ramp_prehud[0]) == 1.0f);
  uint16_t h[4];
  std::memcpy(h, raw.data() + (0 * kW + 3) * 8, 8);
  CHECK(XMConvertHalfToFloat(h[0]) > (pwl ? 0.5f : 1.0f));  // different value with the half PWL
}

}  // namespace

TEST_CASE("hdr: reconstruction against hdr_math", "[hdr]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  CheckReconstruct(*provider, false);
}

TEST_CASE("hdr: reconstruction with the PWL ramp", "[hdr]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  CheckReconstruct(*provider, true);
}

namespace {

// Composition of sdr (16x8) with optional residual and pre_overlay: 10-bit codes per pixel (r, g, b).
std::vector<uint32_t> RunCompose(const xe::ui::d3d12::D3D12Provider& p, gpu::HdrPasses& passes,
                                 ID3D12Resource* sdr, ID3D12Resource* pre_overlay, ID3D12Resource* residual,
                                 const D3D12_RECT& rect, uint32_t use) {
  ID3D12Device* d = p.GetDevice();
  constexpr uint32_t w = 16, h = 8;
  auto out = MakeTexture(d, w, h, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  REQUIRE(SUCCEEDED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))));
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  d->CreateRenderTargetView(out.Get(), nullptr, rtv);
  gow2_test::RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) {
    passes.Compose(l, sdr, pre_overlay, residual, rect, 200, rtv, w, h, 0, use);
  });
  const auto raw = gow2_test::Readback(p, out.Get(), 4, D3D12_RESOURCE_STATE_RENDER_TARGET);
  std::vector<uint32_t> px(w * h);
  std::memcpy(px.data(), raw.data(), raw.size());
  return px;
}

void CheckPq(uint32_t px, hdr::Rgb e) {
  const float ev[3] = {e.r, e.g, e.b};
  for (int c = 0; c < 3; ++c) {
    const int got = int(px >> (10 * c) & 1023), want = int(std::lround(1023.0f * ev[c]));
    CHECK(std::abs(got - want) <= 1);
  }
}

}  // namespace

TEST_CASE("hdr: PQ composition against hdr_math", "[hdr]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::HdrPasses::Create(d);
  REQUIRE(passes);

  constexpr auto kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  const uint32_t gray = Gray10(0.5f);
  const float s = (gray & 1023) / 1023.0f;
  const hdr::Rgb sdr_rgb{s, s, s}, resid_rgb{1, 0.5f, 0};
  auto sdr = MakeTexture(d, 16, 8, DXGI_FORMAT_R10G10B10A2_TYPELESS, kPs);
  auto residual = MakeTexture(d, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, kPs);
  const auto sdr_data = Fill<uint32_t>(16 * 8, {gray});
  const auto resid_data = Fill<uint16_t>(8 * 8, {Half(1), Half(0.5f), Half(0), Half(0)});
  gow2_test::Upload(*provider, sdr.Get(), 4, sdr_data.data(), kPs);
  gow2_test::Upload(*provider, residual.Get(), 8, resid_data.data(), kPs);
  const D3D12_RECT rect{4, 0, 12, 8};
  const hdr::Rgb with = hdr::ComposePq(sdr_rgb, resid_rgb, 200), without = hdr::ComposePq(sdr_rgb, {0, 0, 0}, 200);

  SECTION("residual in the rectangle") {
    const auto px = RunCompose(*provider, *passes, sdr.Get(), nullptr, residual.Get(), rect, 0);
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 16; ++x) {
        INFO("pixel " << x << "," << y);
        CheckPq(px[y * 16 + x], x >= 4 && x < 12 ? with : without);
      }
  }
  SECTION("without residual") {
    const auto px = RunCompose(*provider, *passes, sdr.Get(), nullptr, nullptr, rect, 1);
    for (uint32_t i = 0; i < px.size(); ++i) {
      INFO("pixel " << i % 16 << "," << i / 16);
      CheckPq(px[i], without);
    }
  }
}

TEST_CASE("hdr: composition with the overlay", "[hdr]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::HdrPasses::Create(d);
  REQUIRE(passes);

  constexpr auto kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  const uint32_t gray = Gray10(0.5f);
  const float s = (gray & 1023) / 1023.0f;
  auto sdr = MakeTexture(d, 16, 8, DXGI_FORMAT_R10G10B10A2_UNORM, kPs);
  auto pre_overlay = MakeTexture(d, 16, 8, DXGI_FORMAT_R10G10B10A2_UNORM, kPs);
  auto residual = MakeTexture(d, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, kPs);
  const auto sdr_data = Fill<uint32_t>(16 * 8, {gray});
  auto pre_data = sdr_data;
  auto in_square = [](uint32_t x, uint32_t y) { return x >= 6 && x < 8 && y >= 2 && y < 4; };
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      if (in_square(x, y)) pre_data[y * 16 + x] = Gray10(0.25f);
  const auto resid_data = Fill<uint16_t>(8 * 8, {Half(1), Half(0.5f), Half(0), Half(0)});
  gow2_test::Upload(*provider, sdr.Get(), 4, sdr_data.data(), kPs);
  gow2_test::Upload(*provider, pre_overlay.Get(), 4, pre_data.data(), kPs);
  gow2_test::Upload(*provider, residual.Get(), 8, resid_data.data(), kPs);
  const hdr::Rgb with = hdr::ComposePq({s, s, s}, {1, 0.5f, 0}, 200), without = hdr::ComposePq({s, s, s}, {0, 0, 0}, 200);

  const auto px = RunCompose(*provider, *passes, sdr.Get(), pre_overlay.Get(), residual.Get(), {4, 0, 12, 8}, 0);
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      INFO("pixel " << x << "," << y);
      CheckPq(px[y * 16 + x], x >= 4 && x < 12 && !in_square(x, y) ? with : without);
    }
}

TEST_CASE("hdr: two compositions in the same command list", "[hdr]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::HdrPasses::Create(d);
  REQUIRE(passes);

  constexpr auto kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr uint32_t w = 16, h = 8;
  const uint32_t gray_a = Gray10(0.5f), gray_b = Gray10(0.25f);
  const float sa = (gray_a & 1023) / 1023.0f, sb = (gray_b & 1023) / 1023.0f;
  auto sdr_a = MakeTexture(d, w, h, DXGI_FORMAT_R10G10B10A2_TYPELESS, kPs);
  auto sdr_b = MakeTexture(d, w, h, DXGI_FORMAT_R10G10B10A2_TYPELESS, kPs);
  auto residual = MakeTexture(d, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, kPs);
  const auto data_a = Fill<uint32_t>(w * h, {gray_a}), data_b = Fill<uint32_t>(w * h, {gray_b});
  const auto resid_data = Fill<uint16_t>(8 * 8, {Half(1), Half(0.5f), Half(0), Half(0)});
  gow2_test::Upload(*provider, sdr_a.Get(), 4, data_a.data(), kPs);
  gow2_test::Upload(*provider, sdr_b.Get(), 4, data_b.data(), kPs);
  gow2_test::Upload(*provider, residual.Get(), 8, resid_data.data(), kPs);

  ComPtr<ID3D12Resource> out[2];
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  REQUIRE(SUCCEEDED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))));
  const UINT inc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtv_heap->GetCPUDescriptorHandleForHeapStart(), {}};
  rtv[1].ptr = rtv[0].ptr + inc;
  for (int i = 0; i < 2; ++i) {
    out[i] = MakeTexture(d, w, h, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    d->CreateRenderTargetView(out[i].Get(), nullptr, rtv[i]);
  }
  const D3D12_RECT rect{4, 0, 12, 8};
  gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
    passes->Compose(l, sdr_a.Get(), nullptr, residual.Get(), rect, 200, rtv[0], w, h, 0, 0);
    passes->Compose(l, sdr_b.Get(), nullptr, nullptr, rect, 200, rtv[1], w, h, 0, 1);
  });
  const hdr::Rgb with = hdr::ComposePq({sa, sa, sa}, {1, 0.5f, 0}, 200), a_only = hdr::ComposePq({sa, sa, sa}, {0, 0, 0}, 200),
                 without = hdr::ComposePq({sb, sb, sb}, {0, 0, 0}, 200);
  for (int i = 0; i < 2; ++i) {
    const auto raw = gow2_test::Readback(*provider, out[i].Get(), 4, D3D12_RESOURCE_STATE_RENDER_TARGET);
    std::vector<uint32_t> px(w * h);
    std::memcpy(px.data(), raw.data(), raw.size());
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        INFO("rt " << i << " pixel " << x << "," << y);
        CheckPq(px[y * w + x], i == 1 ? without : x >= 4 && x < 12 ? with : a_only);
      }
  }
}
