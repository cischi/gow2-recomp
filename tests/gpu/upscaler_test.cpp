// Upscaler passes on the GPU (runtime/gpu/upscale_passes.h): motion vectors from UE3's uber post
// compared with the same formula on the CPU; HUD composition and gamma ramp compared with Xenia's
// apply_gamma shaders. SKIP without a hardware D3D12 adapter.
#include <catch2/catch_test_macros.hpp>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "gpu/upscale_passes.h"
#include "gpu_texture_util.h"

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
}  // namespace shaders

namespace {

using gow2_test::MakeTexture;

constexpr uint32_t kW = 64, kH = 32, kVW = 32, kVH = 16;

// Title constants (still camera, from the spike log) as a base.
gpu::UberPostConstants TitleConstants() {
  gpu::UberPostConstants k{};
  const float c2[4] = {9.99f, 0.001f, 0.1001f, 0.0001f}, c13[4] = {1, 0, 0, 0}, c14[4] = {0, 1, 0, 0},
              c15[4] = {0, 0, 0.999f, 1}, c16[4] = {0, 0.000031f, -9.99f, 0}, c17[4] = {0.25f, -0.25f, 0.0125f, 0.007031f};
  std::memcpy(k.c2, c2, 16);
  std::memcpy(k.c13, c13, 16);
  std::memcpy(k.c14, c14, 16);
  std::memcpy(k.c15, c15, 16);
  std::memcpy(k.c16, c16, 16);
  std::memcpy(k.c17, c17, 16);
  return k;
}

// Reference: instructions 8-29 of the uber post (B6B6DD4E7D91F3D9), then pixel from current to previous.
std::array<float, 2> ExpectedMv(uint32_t x, uint32_t y, float z, float vx, float vy, const gpu::UberPostConstants& k) {
  const float u = (x + 0.5f) / kW, v = (y + 0.5f) / kH;
  float disp_x, disp_y;  // UV displacement from previous to current
  if (vx > 0.0f) {
    const float nx = vx * 2 - 1, ny = vy * 2 - 1;
    disp_x = nx * k.c17[2] * 0.5f / k.c17[0];
    disp_y = ny * k.c17[3] * (-0.5f / k.c17[1]);
  } else {
    float d = std::fmin(1.0f / (z * k.c2[2] - k.c2[3]), 65504.0f);
    if (d < 14.0f) d = 65504.0f;
    const float sx = u * 2 - 1, sy = 1 - v * 2;
    auto row = [&](int i) { return d * (sx * k.c13[i] + sy * k.c14[i] + k.c15[i]) + k.c16[i]; };
    const float px = row(0), py = row(1), pw = row(3);
    disp_x = (sx - px / pw) * 0.5f;
    disp_y = -(sy - py / pw) * 0.5f;
  }
  return {-disp_x * kW, -disp_y * kH};
}

struct Inputs {
  std::vector<float> depth = std::vector<float>(kW * kH);
  std::vector<uint16_t> velocity = std::vector<uint16_t>(kVW * kVH * 2);  // R16G16_UNORM
};

std::vector<std::array<float, 2>> RunMv(const xe::ui::d3d12::D3D12Provider& p, gpu::UpscalePasses& passes,
                                        const Inputs& in, const gpu::UberPostConstants& k) {
  ID3D12Device* d = p.GetDevice();
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  auto depth = MakeTexture(d, kW, kH, DXGI_FORMAT_R32_FLOAT, kRead);
  auto velocity = MakeTexture(d, kVW, kVH, DXGI_FORMAT_R16G16_TYPELESS, kRead);
  auto mv = MakeTexture(d, kW, kH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  gow2_test::Upload(p, depth.Get(), 4, in.depth.data(), kRead);
  gow2_test::Upload(p, velocity.Get(), 4, in.velocity.data(), kRead);
  gow2_test::RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) {
    passes.MotionVectors(l, depth.Get(), velocity.Get(), mv.Get(), k, 0);
  });
  const auto raw = gow2_test::Readback(p, mv.Get(), 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  std::vector<std::array<float, 2>> out(kW * kH);
  for (size_t i = 0; i < out.size(); ++i) {
    uint16_t h[2];
    std::memcpy(h, raw.data() + i * 4, 4);
    out[i] = {DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1])};
  }
  return out;
}

bool Near(float a, float b) { return std::fabs(a - b) <= 0.02f + 0.002f * std::fabs(b); }

}  // namespace

TEST_CASE("upscaler: motion vectors from UE3's uber post", "[upscaler]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  auto passes = gpu::UpscalePasses::Create(provider->GetDevice());
  REQUIRE(passes);

  Inputs in;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) in.depth[y * kW + x] = (x + y) % 3 == 0 ? 0.0f : 0.002f + 0.01f * float(x % 7);

  SECTION("still camera: no movement") {
    const auto mv = RunMv(*provider, *passes, in, TitleConstants());
    for (const auto& v : mv) {
      CHECK(std::fabs(v[0]) < 0.01f);
      CHECK(std::fabs(v[1]) < 0.01f);
    }
  }

  SECTION("still camera with the game's numeric noise: no movement") {
    // Real constants of the still scene (path choice, identical for thousands of frames): they are not
    // the identity because of the game's rounding (-2^-7, 2^-10...), which would give ~0.4 px every frame.
    auto k = TitleConstants();
    k.c14[3] = -0.0f;
    k.c15[0] = -0.000164f;
    k.c15[1] = 0.000027f;
    k.c15[2] = 0.999077f;
    k.c15[3] = 1.000077f;
    k.c16[0] = -0.007812f;
    k.c16[1] = -0.000122f;
    k.c16[2] = -9.989746f;
    k.c16[3] = 0.000977f;
    CHECK(gpu::CameraAtRest(k));
    const auto mv = RunMv(*provider, *passes, in, k);
    for (const auto& v : mv) {
      CHECK(std::fabs(v[0]) < 0.01f);
      CHECK(std::fabs(v[1]) < 0.01f);
    }
  }

  SECTION("rotating and translating camera: like the game's formula") {
    CHECK_FALSE(gpu::CameraAtRest([] {
      auto k = TitleConstants();
      k.c15[1] = 0.001139f;  // spike moving frame: slow translation, identical rotation
      k.c16[0] = 0.070312f;
      return k;
    }()));
    auto k = TitleConstants();
    k.c13[1] = -0.000682f;
    k.c14[0] = 0.000216f;
    k.c15[0] = -0.00033f;
    k.c16[0] = -0.617188f;
    k.c16[1] = -0.337982f;
    k.c16[3] = 2.869385f;
    const auto mv = RunMv(*provider, *passes, in, k);
    for (uint32_t y = 0; y < kH; ++y)
      for (uint32_t x = 0; x < kW; ++x) {
        const auto e = ExpectedMv(x, y, in.depth[y * kW + x], 0, 0, k);
        INFO(x << "," << y << " expected " << e[0] << "," << e[1] << " read " << mv[y * kW + x][0] << ","
                << mv[y * kW + x][1]);
        CHECK(Near(mv[y * kW + x][0], e[0]));
        CHECK(Near(mv[y * kW + x][1], e[1]));
      }
  }

  SECTION("velocity buffer of a dynamic object") {
    // Texel (5, 3) of the half-resolution velocity buffer: pixels (10..11, 6..7).
    in.velocity[(3 * kVW + 5) * 2 + 0] = 49151;  // 0.75
    in.velocity[(3 * kVW + 5) * 2 + 1] = 16384;  // 0.25
    const auto k = TitleConstants();
    const auto mv = RunMv(*provider, *passes, in, k);
    const auto e = ExpectedMv(10, 6, 0.0f, 49151 / 65535.0f, 16384 / 65535.0f, k);
    CHECK(std::fabs(e[0]) > 0.5f);  // 0.8 px at 64 px width
    for (uint32_t y = 6; y < 8; ++y)
      for (uint32_t x = 10; x < 12; ++x) {
        CHECK(Near(mv[y * kW + x][0], e[0]));
        CHECK(Near(mv[y * kW + x][1], e[1]));
      }
    CHECK(std::fabs(mv[6 * kW + 12][0]) < 0.01f);  // neighboring texel: static, still camera
  }
}

// ---- HUD composition and gamma ramp (UpscalePasses::Compose) ----

namespace {

constexpr uint32_t kCW = 32, kCH = 16;

uint32_t Pack1010102(uint32_t r, uint32_t g, uint32_t b) { return r | g << 10 | b << 20 | 3u << 30; }

// 256-entry table (DC_LUT_30_COLOR: blue 0:9, green 10:19, red 20:29), different curve per channel.
gpu::GammaRamp TableRamp(float exponent) {
  gpu::GammaRamp ramp{};
  ramp.pwl = false;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = uint32_t(std::lround(std::pow(i / 255.0, exponent) * 1023.0));
    ramp.table[i] = v | (1023 - v) << 10 | (v / 2) << 20;
  }
  return ramp;
}

// 128-entry-per-channel PWL (DC_LUT_PWL_DATA: base in the low 16 bits, delta in the high ones, both * 64).
gpu::GammaRamp PwlRamp() {
  gpu::GammaRamp ramp{};
  ramp.pwl = true;
  for (uint32_t i = 0; i < 128; ++i)
    for (uint32_t c = 0; c < 3; ++c) {
      const uint32_t base = std::min(1023u, i * 8 + c * 40), delta = 8 + c;
      ramp.pwl_rgb[i * 3 + c] = (base << 6) | (delta << 6) << 16;
    }
  return ramp;
}

// ReactiveMask's R8 kCW x kCH reactive mask.
std::vector<uint8_t> RunReactive(const xe::ui::d3d12::D3D12Provider& p, gpu::UpscalePasses& passes,
                                 ID3D12Resource* prehud, ID3D12Resource* guest, const gpu::GammaRamp& ramp) {
  auto reactive = MakeTexture(p.GetDevice(), kCW, kCH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  gow2_test::RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) { passes.ReactiveMask(l, prehud, guest, ramp, reactive.Get(), 0); });
  return gow2_test::Readback(p, reactive.Get(), 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// Xenia: apply_gamma_table_cs / apply_gamma_pwl_cs on the same R8G8B8A8 source, R10G10B10A2 output.
std::vector<uint32_t> RunXeniaGamma(const xe::ui::d3d12::D3D12Provider& p, ID3D12Resource* source,
                                    const gpu::GammaRamp& ramp) {
  ID3D12Device* d = p.GetDevice();
  D3D12_DESCRIPTOR_RANGE ranges[3] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0}};
  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 2;
  for (int i = 0; i < 3; ++i) {
    params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i + 1].DescriptorTable = {1, &ranges[i]};
  }
  D3D12_ROOT_SIGNATURE_DESC rsd{4, params};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
  rs.Attach(xe::ui::d3d12::util::CreateRootSignature(p, rsd));
  REQUIRE(rs);
  const void* code = ramp.pwl ? static_cast<const void*>(shaders::apply_gamma_pwl_cs) : shaders::apply_gamma_table_cs;
  const size_t size = ramp.pwl ? sizeof(shaders::apply_gamma_pwl_cs) : sizeof(shaders::apply_gamma_table_cs);
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  pso.Attach(xe::ui::d3d12::util::CreateComputePipeline(d, code, size, rs.Get()));
  REQUIRE(pso);

  auto ramp_buffer = gow2_test::MakeBuffer(d, (256 + 384) * 4, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  REQUIRE(SUCCEEDED(ramp_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
  if (ramp.pwl) std::memcpy(mapped, ramp.pwl_rgb, sizeof(ramp.pwl_rgb));
  else std::memcpy(mapped, ramp.table, sizeof(ramp.table));
  ramp_buffer->Unmap(0, nullptr);
  auto dest = MakeTexture(d, kCW, kCH, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  REQUIRE(SUCCEEDED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
  const UINT inc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = [&](int i) {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * inc;
    return h;
  };
  auto gpu_h = [&](int i) {
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += uint64_t(i) * inc;
    return h;
  };
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  d->CreateUnorderedAccessView(dest.Get(), nullptr, &uav, cpu(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Format = ramp.pwl ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
  srv.Buffer.NumElements = ramp.pwl ? 384 : 256;
  d->CreateShaderResourceView(ramp_buffer.Get(), &srv, cpu(1));
  D3D12_SHADER_RESOURCE_VIEW_DESC tex{};
  tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  tex.Texture2D.MipLevels = 1;
  d->CreateShaderResourceView(source, &tex, cpu(2));
  gow2_test::RunOnGpu(p, [&](ID3D12GraphicsCommandList* l) {
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(rs.Get());
    const uint32_t k[2] = {kCW, kCH};
    l->SetComputeRoot32BitConstants(0, 2, k, 0);
    l->SetComputeRootDescriptorTable(1, gpu_h(0));
    l->SetComputeRootDescriptorTable(2, gpu_h(1));
    l->SetComputeRootDescriptorTable(3, gpu_h(2));
    l->SetPipelineState(pso.Get());
    l->Dispatch((kCW + 15) / 16, (kCH + 7) / 8, 1);
  });
  const auto raw = gow2_test::Readback(p, dest.Get(), 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  std::vector<uint32_t> out(kCW * kCH);
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

}  // namespace

TEST_CASE("upscaler: HUD reactive mask with Xenia's gamma ramp", "[upscaler]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::UpscalePasses::Create(d);
  REQUIRE(passes);
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  std::vector<uint32_t> prehud_px(kCW * kCH);
  for (uint32_t i = 0; i < prehud_px.size(); ++i)
    prehud_px[i] = 0xFF000000u | (i * 37 % 256) | (i * 91 % 256) << 8 | (i * 13 % 256) << 16;
  auto prehud = MakeTexture(d, kCW, kCH, DXGI_FORMAT_R8G8B8A8_UNORM, kRead);
  gow2_test::Upload(*provider, prehud.Get(), 4, prehud_px.data(), kRead);

  for (const gpu::GammaRamp& ramp : {TableRamp(0.8f), PwlRamp()}) {
    INFO((ramp.pwl ? "PWL" : "table"));
    // Guest output = Xenia's ramp applied to the prehud (like IssueSwap): our ramp coincides, null mask.
    auto guest_px = RunXeniaGamma(*provider, prehud.Get(), ramp);
    auto guest = MakeTexture(d, kCW, kCH, DXGI_FORMAT_R10G10B10A2_UNORM, kRead);
    gow2_test::Upload(*provider, guest.Get(), 4, guest_px.data(), kRead);
    for (uint8_t v : RunReactive(*provider, *passes, prehud.Get(), guest.Get(), ramp)) CHECK(v == 0);

    // HUD: white rectangle (8..15, 4..11) in the guest output -> full mask there, null elsewhere.
    for (uint32_t y = 4; y < 12; ++y)
      for (uint32_t x = 8; x < 16; ++x) guest_px[y * kCW + x] = Pack1010102(1023, 1023, 1023);
    gow2_test::Upload(*provider, guest.Get(), 4, guest_px.data(), kRead);
    const auto mask = RunReactive(*provider, *passes, prehud.Get(), guest.Get(), ramp);
    for (uint32_t y = 0; y < kCH; ++y)
      for (uint32_t x = 0; x < kCW; ++x) {
        INFO(x << "," << y);
        const bool hud = x >= 8 && x < 16 && y >= 4 && y < 12;
        // A prehud pixel already almost white may differ little: it is enough that the mask is not null.
        if (hud) CHECK(mask[y * kCW + x] > 0);
        else CHECK(mask[y * kCW + x] == 0);
      }
  }
}

namespace {

constexpr uint32_t kHudlessOw = 1280, kHudlessOh = 720;
const uint32_t kHudlessGray = uint32_t(std::lround(128 * 1023.0 / 255.0));
const uint32_t kHudlessGreen = Pack1010102(0, 1023, 0);

bool GrayWithin4(uint32_t v) {
  auto within4 = [](uint32_t a, uint32_t b) { return a + 4 >= b && a <= b + 4; };
  return within4(v & 1023, kHudlessGray) && within4(v >> 10 & 1023, kHudlessGray) &&
         within4(v >> 20 & 1023, kHudlessGray);
}

// 1280x720 HUD-less in the rect rectangle: 640x360 gray 128 render with white HUD at (0..63, 0..31), green base
// with the same HUD (white at 0..127, 0..63). R10G10B10A2 pixels read.
std::vector<uint32_t> RunHudless(const D3D12_RECT& rect) {
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  auto passes = gpu::UpscalePasses::Create(d);
  REQUIRE(passes);
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr uint32_t kRw = 640, kRh = 360, kOw = kHudlessOw, kOh = kHudlessOh;

  gpu::GammaRamp ramp{};  // table identity
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = uint32_t(std::lround(i * 1023.0 / 255.0));
    ramp.table[i] = Pack1010102(v, v, v);
  }
  const uint32_t gray = kHudlessGray;

  std::vector<uint32_t> prehud_px(kRw * kRh, 0xFF808080u), guest_px(kRw * kRh, Pack1010102(gray, gray, gray));
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 64; ++x) guest_px[y * kRw + x] = Pack1010102(1023, 1023, 1023);
  auto prehud = MakeTexture(d, kRw, kRh, DXGI_FORMAT_R8G8B8A8_UNORM, kRead);
  gow2_test::Upload(*provider, prehud.Get(), 4, prehud_px.data(), kRead);
  auto guest = MakeTexture(d, kRw, kRh, DXGI_FORMAT_R10G10B10A2_UNORM, kRead);
  gow2_test::Upload(*provider, guest.Get(), 4, guest_px.data(), kRead);
  auto reactive = MakeTexture(d, kRw, kRh, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
    passes->ReactiveMask(l, prehud.Get(), guest.Get(), ramp, reactive.Get(), 0);
  });
  gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
    gow2_test::Barrier(l, reactive.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
  });

  std::vector<uint32_t> base_px(kOw * kOh, kHudlessGreen);
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 128; ++x) base_px[y * kOw + x] = Pack1010102(1023, 1023, 1023);
  auto base = MakeTexture(d, kOw, kOh, DXGI_FORMAT_R10G10B10A2_UNORM, kRead);
  gow2_test::Upload(*provider, base.Get(), 4, base_px.data(), kRead);
  auto out = MakeTexture(d, kOw, kOh, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  gow2_test::RunOnGpu(*provider, [&](ID3D12GraphicsCommandList* l) {
    passes->Hudless(l, base.Get(), prehud.Get(), reactive.Get(), false, rect, out.Get(), 0);
  });
  const auto raw = gow2_test::Readback(*provider, out.Get(), 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  std::vector<uint32_t> px(size_t(kOw) * kOh);
  std::memcpy(px.data(), raw.data(), px.size() * 4);
  return px;
}

}  // namespace

TEST_CASE("upscaler: output HUD-less removes the HUD", "[upscaler]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  const auto px = RunHudless({0, 0, LONG(kHudlessOw), LONG(kHudlessOh)});
  for (auto [x, y] : std::array<std::pair<uint32_t, uint32_t>, 2>{{{10, 10}, {100, 40}}}) {
    INFO(x << "," << y);
    CHECK(GrayWithin4(px[y * kHudlessOw + x]));
  }
  for (auto [x, y] : std::array<std::pair<uint32_t, uint32_t>, 2>{{{640, 360}, {400, 200}}}) {
    INFO(x << "," << y);
    CHECK(px[y * kHudlessOw + x] == kHudlessGreen);
  }
}

// Image with black bands (fit, different aspect): black HUD-less outside the rectangle, uv relative to the rectangle inside.
TEST_CASE("upscaler: HUD-less in the image rectangle, black in the bands", "[upscaler]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  const auto px = RunHudless({160, 0, 1120, 720});  // 960x720 in the center
  CHECK(px[360 * kHudlessOw + 50] == 3u << 30);      // band: opaque black, like the presenter's clear
  CHECK(px[360 * kHudlessOw + 1200] == 3u << 30);
  CHECK(px[360 * kHudlessOw + 640] == kHudlessGreen);  // inside, far from the HUD: base
  CHECK(GrayWithin4(px[20 * kHudlessOw + 175]));       // HUD (render 10,10 -> 160 + 15, 20): gray prehud
}

TEST_CASE("upscaler: mip bias log2(render/output), never positive", "[gpu][upscaler]") {
  CHECK(gpu::UpscalerMipBias(1280, 2560) == -1.0f);
  CHECK(std::abs(gpu::UpscalerMipBias(1280, 1920) - std::log2(2.0f / 3.0f)) < 1e-6f);
  CHECK(gpu::UpscalerMipBias(1280, 1280) == 0.0f);
  CHECK(gpu::UpscalerMipBias(1280, 960) == 0.0f);
  CHECK(gpu::UpscalerMipBias(0, 1920) == 0.0f);
  CHECK(gpu::UpscalerMipBias(1280, 0) == 0.0f);
}

TEST_CASE("upscaler: size to copy of a guest texture, scaled or native", "[gpu][upscaler]") {
  // Scaled: the resource is guest * scale wide.
  CHECK(gpu::CopySizeOfGuestTexture(1280, 2, 2560) == 2560u);
  CHECK(gpu::CopySizeOfGuestTexture(720, 2, 1440) == 1440u);
  // Native (draw_resolution_scale_threshold, buffers up to 640 px): the resource is as wide as the guest.
  CHECK(gpu::CopySizeOfGuestTexture(640, 2, 640) == 640u);
  CHECK(gpu::CopySizeOfGuestTexture(640, 3, 640) == 640u);
  // Tile alignment of the resource: what matters is that it reaches guest * scale.
  CHECK(gpu::CopySizeOfGuestTexture(1270, 2, 2560) == 2540u);
  CHECK(gpu::CopySizeOfGuestTexture(640, 1, 640) == 640u);
}

TEST_CASE("upscaler: ShaderViewFormat", "[upscaler]") {
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS) == DXGI_FORMAT_R10G10B10A2_UNORM);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R16G16_TYPELESS) == DXGI_FORMAT_R16G16_UNORM);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R32_TYPELESS) == DXGI_FORMAT_R32_FLOAT);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R24G8_TYPELESS) == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R32G8X24_TYPELESS) == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
  CHECK(gpu::ShaderViewFormat(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT);
}
