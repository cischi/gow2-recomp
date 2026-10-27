// Gow2 DXGI presenter (gpu/presenter.h) on an invisible window: guest
// output filled by a refresher like D3D12CommandProcessor::IssueSwap does,
// Present, BMP screenshot from the back buffer; fit/stretch scaling and back buffer
// following the client area.
//
// Skipped (SKIP) without a hardware D3D12 adapter (gpu::FindHardwareAdapter).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "gpu/d3d12_probe.h"
#include "gpu/frame_generation.h"
#include "gpu/overlay.h"
#include "gpu/presenter.h"
#include "gpu_texture_util.h"
#include "hdr_math.h"
#include "log.h"
#include "platform/platform.h"
#include "settings.h"
#include "temp_dir.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace {

using Microsoft::WRL::ComPtr;
using xe::ui::d3d12::D3D12Presenter;

std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

uint32_t Le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

// Counts the pixels of the 24-bit BMP (bottom-up, rows aligned to 4 bytes)
// different from (r, g, b).
size_t PixelsDifferentFrom(const std::vector<uint8_t>& bmp, uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t w = Le32(bmp, 18), h = Le32(bmp, 22), off = Le32(bmp, 10);
  const uint32_t pitch = (w * 3 + 3) & ~3u;
  size_t different = 0;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = bmp.data() + off + size_t(y) * pitch;
    for (uint32_t x = 0; x < w; ++x)
      if (row[x * 3] != b || row[x * 3 + 1] != g || row[x * 3 + 2] != r) ++different;
  }
  return different;
}

// Fills the guest output resource with R10G10B10A2 texels that depend
// only on the column, on the provider's direct queue (like Xenia's
// refresher), and returns it to the input state (kGuestOutputInternalState)
// before returning.
bool FillGuestOutput(const xe::ui::d3d12::D3D12Provider& provider, ID3D12Resource* target,
                     const std::function<uint32_t(uint32_t x)>& texel_at) {
  ID3D12Device* device = provider.GetDevice();
  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

  D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> upload;
  if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
    return false;
  uint8_t* mapped = nullptr;
  if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
  for (UINT y = 0; y < footprint.Footprint.Height; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
    for (UINT x = 0; x < footprint.Footprint.Width; ++x) row[x] = texel_at(x);
  }
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    return false;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = target;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12Presenter::kGuestOutputInternalState;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION dst{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  src.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list->ResourceBarrier(1, &barrier);
  if (FAILED(list->Close())) return false;
  ID3D12CommandList* lists[] = {list.Get()};
  provider.GetDirectQueue()->ExecuteCommandLists(1, lists);
  if (FAILED(provider.GetDirectQueue()->Signal(fence.Get(), 1))) return false;
  // null hEvent: returns when the fence reaches 1
  return SUCCEEDED(fence->SetEventOnCompletion(1, nullptr));
}

bool FillGuestOutput(const xe::ui::d3d12::D3D12Provider& provider, ID3D12Resource* target, uint32_t texel) {
  return FillGuestOutput(provider, target, [texel](uint32_t) { return texel; });
}

}  // namespace

TEST_CASE("presenter: red guest output presented and saved to BMP, re-presented without refresh",
          "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  INFO("adapter: " << *adapter);

  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter");
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window));
    REQUIRE(presenter);
    CHECK(presenter->FramesPresented() == 0);

    // R = 1023 (bit 0..9), G = B = 0, A = 3
    const uint32_t red = 0x3FFu | (3u << 30);
    bool refresher_called = false;
    CHECK(presenter->RefreshGuestOutput(
        1280, 720, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
          refresher_called = true;
          auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
          ID3D12Resource* resource = d3d12_context.resource_uav_capable();
          if (!resource) return false;
          const D3D12_RESOURCE_DESC desc = resource->GetDesc();
          CHECK(desc.Format == D3D12Presenter::kGuestOutputFormat);
          CHECK(desc.Width == 1280);
          CHECK(desc.Height == 720);
          CHECK((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
          return FillGuestOutput(*provider, resource, red);
        }));
    CHECK(refresher_called);

    const auto red_file = dir.path / "red.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), red_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 1);
    const auto red_bmp = ReadAll(red_file);
    REQUIRE(red_bmp.size() == 54 + 1280 * 720 * 3);
    CHECK(Le32(red_bmp, 18) == 1280);
    CHECK(Le32(red_bmp, 22) == 720);
    CHECK(PixelsDifferentFrom(red_bmp, 255, 0, 0) == 0);

    // Present without a new refresh: the last guest output is re-presented
    // (IssueSwap presents also when the front buffer is not available).
    const auto again_file = dir.path / "red_again.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), again_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 2);
    const auto again_bmp = ReadAll(again_file);
    REQUIRE(again_bmp.size() == 54 + 1280 * 720 * 3);
    CHECK(PixelsDifferentFrom(again_bmp, 255, 0, 0) == 0);

    // Half-scale values: 10 bit -> 8 bit with >> 2 (512 -> 128, 256 -> 64, 4 -> 1).
    const uint32_t mid = 512u | (256u << 10) | (4u << 20) | (3u << 30);
    CHECK(presenter->RefreshGuestOutput(1280, 720, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
      auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
      return FillGuestOutput(*provider, d3d12_context.resource_uav_capable(), mid);
    }));
    const auto mid_file = dir.path / "mid.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), mid_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 3);
    const auto mid_bmp = ReadAll(mid_file);
    REQUIRE(mid_bmp.size() == 54 + 1280 * 720 * 3);
    CHECK(PixelsDifferentFrom(mid_bmp, 128, 64, 1) == 0);

    // screenshot armed in the future: no file
    const auto later_file = dir.path / "later.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now() + std::chrono::hours(1), later_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 4);
    CHECK_FALSE(std::filesystem::exists(later_file));
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: without a successful refresh the frame is black; failing refresher or zero dimensions -> false",
          "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_black");
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window));
    REQUIRE(presenter);
    // never a refresh: black
    const auto black_file = dir.path / "black.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), black_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 1);
    const auto black_bmp = ReadAll(black_file);
    REQUIRE(black_bmp.size() == 54 + 1280 * 720 * 3);
    CHECK(PixelsDifferentFrom(black_bmp, 0, 0, 0) == 0);

    CHECK_FALSE(presenter->RefreshGuestOutput(640, 360, 16, 9, [](auto&) { return false; }));
    bool called = false;
    CHECK_FALSE(presenter->RefreshGuestOutput(0, 720, 16, 9, [&](auto&) { return called = true; }));
    CHECK_FALSE(called);
    // failed refresher (texture maybe half-written): still black
    const auto failed_file = dir.path / "failed.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), failed_file);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == 2);
    const auto failed_bmp = ReadAll(failed_file);
    REQUIRE(failed_bmp.size() == 54 + 1280 * 720 * 3);
    CHECK(PixelsDifferentFrom(failed_bmp, 0, 0, 0) == 0);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: queued screenshots, each served by the first Present from its instant, in order",
          "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_queue");
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window));
    REQUIRE(presenter);
    const auto now = std::chrono::steady_clock::now();
    const auto later_file = dir.path / "later.bmp";
    const auto first_file = dir.path / "now.bmp";
    // armed out of order: the instant counts, not the order of ArmScreenshot
    presenter->ArmScreenshot(now + std::chrono::milliseconds(200), later_file);
    presenter->ArmScreenshot(now, first_file);

    REQUIRE(presenter->Present());
    CHECK(std::filesystem::exists(first_file));
    CHECK_FALSE(std::filesystem::exists(later_file));
    const uint64_t first_frame = presenter->FramesPresented();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(later_file) && std::chrono::steady_clock::now() < deadline)
      REQUIRE(presenter->Present());
    REQUIRE(std::filesystem::exists(later_file));
    CHECK(presenter->FramesPresented() > first_frame);  // produced by a later Present
    CHECK(std::chrono::steady_clock::now() >= now + std::chrono::milliseconds(200));
    CHECK(ReadAll(first_file).size() == 54 + 1280 * 720 * 3);
    CHECK(ReadAll(later_file).size() == 54 + 1280 * 720 * 3);

    // two requests already due: one per Present
    const auto a = dir.path / "a.bmp", b = dir.path / "b.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), a);
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), b);
    REQUIRE(presenter->Present());
    CHECK(std::filesystem::exists(a));
    CHECK_FALSE(std::filesystem::exists(b));
    REQUIRE(presenter->Present());
    CHECK(std::filesystem::exists(b));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: FitRect", "[presenter]") {
  CHECK(gpu::FitRect(1280, 720, 1920, 1080, false) == gpu::Rect{0, 0, 1920, 1080});
  CHECK(gpu::FitRect(1280, 720, 1920, 1200, false) == gpu::Rect{0, 60, 1920, 1080});
  // height 1000 * 720 / 1280 = 562.5 truncated; y = (1000 - 562) / 2
  CHECK(gpu::FitRect(1280, 720, 1000, 1000, false) == gpu::Rect{0, 219, 1000, 562});
  // destination wider than the aspect: bands at the sides
  CHECK(gpu::FitRect(1280, 720, 2560, 1080, false) == gpu::Rect{320, 0, 1920, 1080});
  CHECK(gpu::FitRect(1280, 720, 1920, 1200, true) == gpu::Rect{0, 0, 1920, 1200});
  CHECK(gpu::FitRect(1280, 720, 0, 0, false) == gpu::Rect{0, 0, 0, 0});
  CHECK(gpu::FitRect(1280, 720, 0, 0, true) == gpu::Rect{0, 0, 0, 0});
}

namespace {

struct Rgb {
  int r, g, b;
};

// Pixel (x, y) from the top of the bottom-up 24-bit BMP.
Rgb PixelAt(const std::vector<uint8_t>& bmp, uint32_t x, uint32_t y) {
  const uint32_t w = Le32(bmp, 18), h = Le32(bmp, 22), off = Le32(bmp, 10);
  const uint32_t pitch = (w * 3 + 3) & ~3u;
  const uint8_t* p = bmp.data() + off + size_t(h - 1 - y) * pitch + size_t(x) * 3;
  return {p[2], p[1], p[0]};
}

bool Near(Rgb a, Rgb b) { return std::abs(a.r - b.r) <= 1 && std::abs(a.g - b.g) <= 1 && std::abs(a.b - b.b) <= 1; }

// R = 512, G = 256, B = 4 (10 bit) -> 128, 64, 1 at 8 bit.
constexpr uint32_t kMidTexel = 512u | (256u << 10) | (4u << 20) | (3u << 30);
constexpr Rgb kMid{128, 64, 1};

bool RefreshMid(gpu::Presenter& presenter, const xe::ui::d3d12::D3D12Provider& provider) {
  return presenter.RefreshGuestOutput(1280, 720, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    return FillGuestOutput(provider, d3d12_context.resource_uav_capable(), kMidTexel);
  });
}

std::vector<uint8_t> Shoot(gpu::Presenter& presenter, const std::filesystem::path& file) {
  presenter.ArmScreenshot(std::chrono::steady_clock::now(), file);
  if (!presenter.Present()) return {};
  return ReadAll(file);
}

// Window client area: SetWindowPos sends WM_SIZE to the window thread
// and waits.
void SetClientSize(void* hwnd, int w, int h) {
  HWND wnd = static_cast<HWND>(hwnd);
  RECT r{0, 0, w, h};
  AdjustWindowRectEx(&r, DWORD(GetWindowLongW(wnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(wnd, GWL_EXSTYLE)));
  SetWindowPos(wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

TEST_CASE("presenter: 1280x720 guest output scaled with fit/stretch on a fixed-size back buffer",
          "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  // Fixed back buffer (PresentOptions), independent of the window and the monitor.
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_scaling");
  {
    gpu::PresentOptions options;
    options.width = 1920;
    options.height = 1080;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(RefreshMid(*presenter, *provider));
    const auto bmp = Shoot(*presenter, dir.path / "1080.bmp");
    REQUIRE(bmp.size() == 54 + 1920 * 1080 * 3);
    CHECK(Near(PixelAt(bmp, 960, 540), kMid));
    CHECK(Near(PixelAt(bmp, 0, 0), kMid));  // 16:9 on 16:9: no bands
    CHECK(Near(PixelAt(bmp, 1919, 1079), kMid));
  }
  {
    gpu::PresentOptions options;
    options.width = 1920;
    options.height = 1200;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(RefreshMid(*presenter, *provider));
    const auto bmp = Shoot(*presenter, dir.path / "fit.bmp");
    REQUIRE(bmp.size() == 54 + 1920 * 1200 * 3);
    CHECK(Near(PixelAt(bmp, 960, 600), kMid));
    CHECK(Near(PixelAt(bmp, 0, 60), kMid));  // first row of the image
    CHECK(Near(PixelAt(bmp, 1919, 1139), kMid));  // last row of the image
    for (const auto [x, y] : {std::pair{0u, 0u}, {1919u, 0u}, {0u, 1199u}, {1919u, 1199u}, {960u, 59u}, {960u, 1140u}})
      CHECK(Near(PixelAt(bmp, x, y), Rgb{0, 0, 0}));
  }
  {
    gpu::PresentOptions options;
    options.width = 1920;
    options.height = 1200;
    options.stretch = true;
    options.vsync = false;  // Present(0, ...) with or without tearing
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(RefreshMid(*presenter, *provider));
    const auto bmp = Shoot(*presenter, dir.path / "stretch.bmp");
    REQUIRE(bmp.size() == 54 + 1920 * 1200 * 3);
    CHECK(PixelsDifferentFrom(bmp, 128, 64, 1) == 0);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: the back buffer follows the client area; minimized -> Present skipped", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  // vsync off: swap chain with ALLOW_TEARING if supported, which ResizeBuffers
  // must pass again (different flags -> DXGI_ERROR_INVALID_CALL).
  const bool vsync = GENERATE(true, false);
  INFO("vsync " << vsync);
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::WindowOptions window_options;
  window_options.resizable = true;
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", window_options, false, [] {});
  REQUIRE(window);
  void* hwnd = plat::NativeHandle(window);
  gow2_test::TempDir dir("gow2_presenter_resize");
  {
    gpu::PresentOptions options;
    options.vsync = vsync;
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    REQUIRE(RefreshMid(*presenter, *provider));
    const auto first = Shoot(*presenter, dir.path / "first.bmp");
    REQUIRE(first.size() == 54 + 1280 * 720 * 3);
    CHECK(PixelsDifferentFrom(first, 128, 64, 1) == 0);  // 1:1, CopyTextureRegion

    SetClientSize(hwnd, 800, 600);
    const auto resized = Shoot(*presenter, dir.path / "800x600.bmp");
    REQUIRE(resized.size() >= 54);
    CHECK(Le32(resized, 18) == 800);
    CHECK(Le32(resized, 22) == 600);
    // fit: 800 x 450 at y = 75
    CHECK(Near(PixelAt(resized, 400, 300), kMid));
    CHECK(Near(PixelAt(resized, 400, 0), Rgb{0, 0, 0}));
    CHECK(Near(PixelAt(resized, 400, 599), Rgb{0, 0, 0}));

    // Minimized (client area 0x0): no ResizeBuffers, frame not counted,
    // the screenshot stays in the queue.
    const uint64_t frames = presenter->FramesPresented();
    ShowWindow(static_cast<HWND>(hwnd), SW_SHOWMINNOACTIVE);
    const auto pending = dir.path / "after_restore.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), pending);
    CHECK(presenter->Present());
    CHECK(presenter->FramesPresented() == frames);
    CHECK_FALSE(std::filesystem::exists(pending));
    ShowWindow(static_cast<HWND>(hwnd), SW_SHOWNOACTIVATE);  // restored, without focus
    ShowWindow(static_cast<HWND>(hwnd), SW_HIDE);
    REQUIRE(presenter->Present());
    CHECK(presenter->FramesPresented() == frames + 1);
    REQUIRE(std::filesystem::exists(pending));
    CHECK(Le32(ReadAll(pending), 18) == 800);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: ChoosePaintEffect", "[presenter]") {
  using gpu::PaintEffect;
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 1280, 720}, false) == PaintEffect::kBilinear);
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 1280, 720}, true) == PaintEffect::kCasSharpen);
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 2560, 1440}, true) == PaintEffect::kCasResample);
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 640, 360}, true) == PaintEffect::kCasResample);
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 2561, 1440}, true) == PaintEffect::kBilinear);
  CHECK(gpu::ChoosePaintEffect(1280, 720, {0, 0, 639, 360}, true) == PaintEffect::kBilinear);
}

namespace {

// Gray v (0..1) at 10 bit on R, G and B.
uint32_t Gray10(float v) {
  const uint32_t c = uint32_t(std::lround(v * 1023.0f));
  return c | c << 10 | c << 20 | 3u << 30;
}

// Vertical soft edge at the middle of an output w wide: 0.2 | 0.35 | 0.65 | 0.8.
uint32_t SoftEdge(uint32_t w, uint32_t x) {
  return Gray10(x < w / 2 ? 0.2f : x == w / 2 ? 0.35f : x == w / 2 + 1 ? 0.65f : 0.8f);
}

int Gray8(float v) { return int(uint32_t(std::lround(v * 1023.0f)) >> 2); }

bool RefreshWith(gpu::Presenter& presenter, const xe::ui::d3d12::D3D12Provider& provider, uint32_t w, uint32_t h,
                 const std::function<uint32_t(uint32_t x)>& texel_at) {
  return presenter.RefreshGuestOutput(w, h, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    return FillGuestOutput(provider, d3d12_context.resource_uav_capable(), texel_at);
  });
}

// BMP pixels farther than 1 (8 bit) from c, excluding a frame margin wide.
size_t PixelsFarFrom(const std::vector<uint8_t>& bmp, Rgb c, uint32_t margin = 0) {
  const uint32_t w = Le32(bmp, 18), h = Le32(bmp, 22);
  size_t count = 0;
  for (uint32_t y = margin; y + margin < h; ++y)
    for (uint32_t x = margin; x + margin < w; ++x) count += !Near(PixelAt(bmp, x, y), c);
  return count;
}

}  // namespace

// The screenshot is 8 bit (v >> 2): the 10-bit CAS thresholds (2/1023, 1/1023)
// become 2/255 and 1/255.
TEST_CASE("presenter: CAS at 1:1 sharpens a soft edge", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_cas");
  {
    gpu::PresentOptions options;
    options.width = 1280;
    options.height = 720;
    options.sharpening = true;
    options.sharpness = 1.0f;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, [](uint32_t x) { return SoftEdge(1280, x); }));
    const auto bmp = Shoot(*presenter, dir.path / "cas.bmp");
    REQUIRE(bmp.size() == 54 + 1280 * 720 * 3);
    const Rgb dark = PixelAt(bmp, 640, 360), light = PixelAt(bmp, 641, 360);
    INFO("640: " << dark.r << " (input " << Gray8(0.35f) << "), 641: " << light.r << " (input " << Gray8(0.65f)
                 << ")");
    for (const int v : {dark.r, dark.g, dark.b}) CHECK(v / 255.0 < 0.35 - 2 / 255.0);
    for (const int v : {light.r, light.g, light.b}) CHECK(v / 255.0 > 0.65 + 2 / 255.0);
    const int lo = Gray8(0.2f), hi = Gray8(0.8f);
    CHECK(Near(PixelAt(bmp, 100, 360), Rgb{lo, lo, lo}));
    CHECK(Near(PixelAt(bmp, 1200, 360), Rgb{hi, hi, hi}));
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: CAS with resample 1280x720 -> 1920x1080 keeps uniform areas", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_cas_resample");
  {
    gpu::PresentOptions options;
    options.width = 1920;
    options.height = 1080;
    options.sharpening = true;
    options.sharpness = 1.0f;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    const uint32_t r = uint32_t(std::lround(0.5f * 1023)), g = uint32_t(std::lround(0.25f * 1023)),
                   b = uint32_t(std::lround(0.75f * 1023));
    const uint32_t texel = r | g << 10 | b << 20 | 3u << 30;
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, [&](uint32_t) { return texel; }));
    const auto bmp = Shoot(*presenter, dir.path / "resample.bmp");
    REQUIRE(bmp.size() == 54 + 1920 * 1080 * 3);
    const Rgb want{int(r >> 2), int(g >> 2), int(b >> 2)};
    // Xenia's shader reads texels outside the source without clamp (Load
    // -> 0): when enlarging, the rectangle's 1-pixel frame comes out darker.
    INFO("frame: " << PixelsFarFrom(bmp, want) << " different pixels, (0, 0) = " << PixelAt(bmp, 0, 0).r << ", (960, 0) = "
                     << PixelAt(bmp, 960, 0).r);
    CHECK(PixelsFarFrom(bmp, want, 1) == 0);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: CAS beyond 2x falls back to bilinear", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_cas_3x");
  {
    std::vector<uint8_t> shots[2];
    for (const bool sharpening : {false, true}) {
      gpu::PresentOptions options;
      options.width = 1920;
      options.height = 1080;
      options.sharpening = sharpening;
      options.sharpness = 1.0f;
      auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
      REQUIRE(presenter);
      REQUIRE(RefreshWith(*presenter, *provider, 640, 360, [](uint32_t x) { return SoftEdge(640, x); }));
      shots[sharpening] = Shoot(*presenter, dir.path / (sharpening ? "cas.bmp" : "bilinear.bmp"));
      REQUIRE(shots[sharpening].size() == 54 + 1920 * 1080 * 3);
    }
    CHECK(shots[0] == shots[1]);
  }
  plat::DestroyGameWindow(window);
}

// ---- Upscaler ----

namespace {

// Fake upscaler: the output becomes a copy of `fill` (green), fixed jitter.
class FakeUpscaler final : public gpu::Upscaler {
 public:
  explicit FakeUpscaler(Microsoft::WRL::ComPtr<ID3D12Resource> fill) : fill_(std::move(fill)) {}
  const std::string& Name() const override { return name_; }
  uint32_t JitterPhaseCount(uint32_t, uint32_t) override { return 8; }
  void JitterOffset(uint32_t, uint32_t, float& x, float& y) override {
    x = 0.25f;
    y = -0.25f;
  }
  bool Dispatch(const gpu::UpscaleFrame& f) override {
    ++dispatches;
    last = f;
    if (fail) return false;
    gow2_test::Barrier(f.cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    f.cmd->CopyResource(f.output, fill_.Get());
    if (copy_color) {  // color received at the top left of the output (same format), in the contract's state
      constexpr auto kRead =
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      gow2_test::Barrier(f.cmd, f.color, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{f.output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
      D3D12_TEXTURE_COPY_LOCATION src{f.color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
      f.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      gow2_test::Barrier(f.cmd, f.color, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    }
    gow2_test::Barrier(f.cmd, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }
  int dispatches = 0;
  bool fail = false, copy_color = false;
  gpu::UpscaleFrame last{};

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> fill_;
  std::string name_ = "fake";
};

size_t PixelsDifferentIn(const std::vector<uint8_t>& bmp, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                         uint8_t r, uint8_t g, uint8_t b, bool inside) {
  const uint32_t w = Le32(bmp, 18), h = Le32(bmp, 22), off = Le32(bmp, 10);
  const uint32_t pitch = (w * 3 + 3) & ~3u;
  size_t different = 0;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = bmp.data() + off + size_t(h - 1 - y) * pitch;  // BMP from the bottom
    for (uint32_t x = 0; x < w; ++x) {
      const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
      if (in != inside) continue;
      if (row[x * 3] != b || row[x * 3 + 1] != g || row[x * 3 + 2] != r) ++different;
    }
  }
  return different;
}

constexpr uint32_t kRW = 640, kRH = 360;

// Capture kRW x kRH and guest output of the same size (refresh succeeded); guest_output receives its resource.
gpu::UpscaleCapture MakeCapture(gpu::Presenter& presenter, const xe::ui::d3d12::D3D12Provider& provider,
                                ID3D12Resource** guest_output = nullptr) {
  ID3D12Device* d = provider.GetDevice();
  // 640x360 capture: gray 128 prehud, zero depth and velocity (still camera), COPY_DEST state.
  gpu::UpscaleCapture cap{};
  cap.prehud = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
  cap.depth = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
  cap.velocity = gow2_test::MakeTexture(d, kRW / 2, kRH / 2, DXGI_FORMAT_R16G16_TYPELESS, D3D12_RESOURCE_STATE_COPY_DEST);
  const std::vector<uint32_t> gray(kRW * kRH, 0xFF808080u);
  gow2_test::Upload(provider, cap.prehud.Get(), 4, gray.data(), D3D12_RESOURCE_STATE_COPY_DEST);
  const std::vector<uint32_t> zeros(kRW * kRH, 0);
  gow2_test::Upload(provider, cap.depth.Get(), 4, zeros.data(), D3D12_RESOURCE_STATE_COPY_DEST);
  gow2_test::Upload(provider, cap.velocity.Get(), 4, zeros.data(), D3D12_RESOURCE_STATE_COPY_DEST);
  const float c13[4] = {1, 0, 0, 0}, c14[4] = {0, 1, 0, 0}, c15[4] = {0, 0, 0.999f, 1}, c17[4] = {0.25f, -0.25f, 0.0125f, 0.007031f};
  std::memcpy(cap.k.c13, c13, 16);
  std::memcpy(cap.k.c14, c14, 16);
  std::memcpy(cap.k.c15, c15, 16);
  std::memcpy(cap.k.c17, c17, 16);
  cap.k.c2[2] = 0.1001f;
  cap.k.c2[3] = 0.0001f;
  // Identity table ramp: 8 bit -> 10 bit.
  cap.ramp.pwl = false;
  auto to10 = [](uint32_t i) { return uint32_t(std::lround(i * 1023.0 / 255.0)); };
  for (uint32_t i = 0; i < 256; ++i) cap.ramp.table[i] = to10(i) | to10(i) << 10 | to10(i) << 20;

  // Guest output = ramp(prehud), with a white "HUD" at (0..63, 0..31).
  const uint32_t g10 = to10(128);
  std::vector<uint32_t> guest(kRW * kRH, g10 | g10 << 10 | g10 << 20 | 3u << 30);
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 64; ++x) guest[y * kRW + x] = 0xFFFFFFFFu;
  REQUIRE(presenter.RefreshGuestOutput(kRW, kRH, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    if (guest_output) *guest_output = d3d12_context.resource_uav_capable();
    gow2_test::Upload(provider, d3d12_context.resource_uav_capable(), 4, guest.data(),
                      D3D12Presenter::kGuestOutputInternalState);
    return true;
  }));
  return cap;
}

}  // namespace

TEST_CASE("presenter: upscaler, guest output with the HUD rebuilt by the upscaler", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  plat::Window* window = plat::CreateGameWindow("gow2 presenter upscaler", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_upscaler");
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window));
    REQUIRE(presenter);

    ID3D12Resource* guest_output = nullptr;
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider, &guest_output);

    FakeUpscaler* fake = nullptr;
    uint32_t asked[4] = {};
    presenter->SetUpscaler(
        [&](uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) -> std::unique_ptr<gpu::Upscaler> {
          asked[0] = rw, asked[1] = rh, asked[2] = ow, asked[3] = oh;
          auto fill = gow2_test::MakeTexture(d, ow, oh, D3D12Presenter::kGuestOutputFormat,
                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
          const std::vector<uint32_t> green(size_t(ow) * oh, 1023u << 10 | 3u << 30);
          gow2_test::Upload(*provider, fill.Get(), 4, green.data(), D3D12_RESOURCE_STATE_COPY_SOURCE);
          auto u = std::make_unique<FakeUpscaler>(fill);
          fake = u.get();
          return u;
        },
        0.5f, gpu::UpscaleDebug::kNone);
    CHECK(presenter->UpscalerActive());
    CHECK(presenter->NextJitter() == std::pair<float, float>{0.0f, 0.0f});  // context not yet created
    CHECK(presenter->MipBias() == 0.0f);

    presenter->SetUpscaleCapture(&cap);
    const auto file = dir.path / "upscaler.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), file);
    REQUIRE(presenter->Present());
    REQUIRE(fake);
    CHECK(asked[0] == kRW);
    CHECK(asked[1] == kRH);
    CHECK(asked[2] == 1280);
    CHECK(asked[3] == 720);
    CHECK(fake->dispatches == 1);
    CHECK(fake->last.reset);
    CHECK(fake->last.render_w == kRW);
    CHECK(fake->last.output_w == 1280);
    CHECK(fake->last.sharpness == 0.5f);
    // Color = scene without HUD (not the guest output: with the HUD inside the upscaler dragged the lettering along).
    CHECK(fake->last.color != nullptr);
    CHECK(fake->last.color != guest_output);
    CHECK(fake->last.reactive != nullptr);
    const auto bmp = ReadAll(file);
    REQUIRE(bmp.size() == 54 + 1280 * 720 * 3);
    // HUD (0..63, 0..31 of render = 0..127, 0..63 of output) from the guest output, white; the rest from the upscaler
    // (green), outside the edge of the mask dilated by one render texel.
    CHECK(PixelsDifferentIn(bmp, 0, 0, 124, 60, 255, 255, 255, true) == 0);
    CHECK(PixelsDifferentIn(bmp, 0, 0, 136, 72, 0, 255, 0, false) == 0);
    CHECK(presenter->NextJitter() == std::pair<float, float>{0.25f, -0.25f});
    CHECK(presenter->MipBias() == -1.0f);  // 640 -> 1280

    // Frame without capture (loading): guest output as today (gray in the center), then reset.
    presenter->SetUpscaleCapture(nullptr);
    const auto plain = dir.path / "no_capture.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), plain);
    REQUIRE(presenter->Present());
    CHECK(fake->dispatches == 1);
    CHECK(PixelsDifferentIn(ReadAll(plain), 400, 300, 800, 500, 128, 128, 128, true) == 0);
    // Frame not reconstructed: the next one without jitter (guest output shown as is, without shaking).
    CHECK(presenter->NextJitter() == std::pair<float, float>{0.0f, 0.0f});
    CHECK(presenter->MipBias() == 0.0f);
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    CHECK(fake->dispatches == 2);
    CHECK(fake->last.reset);
    REQUIRE(presenter->Present());  // the same capture re-presented is not redone: no new dispatch
    CHECK(fake->dispatches == 2);

    // Empty client area (like minimized, but the test window stays hidden): frames skipped, the upscaler's
    // history is old; on restore reset and no jitter before.
    const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
    SetClientSize(hwnd, 1280, 0);
    RECT client{};
    REQUIRE(GetClientRect(hwnd, &client));
    REQUIRE(client.bottom - client.top == 0);
    presenter->SetUpscaleCapture(&cap);
    CHECK(presenter->Present());
    CHECK(fake->dispatches == 2);
    CHECK(presenter->NextJitter() == std::pair<float, float>{0.0f, 0.0f});
    SetClientSize(hwnd, 1280, 720);
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    CHECK(fake->dispatches == 3);
    CHECK(fake->last.reset);

    // Dispatch failed: upscaler off forever, no jitter, guest output.
    fake->fail = true;
    presenter->SetUpscaleCapture(&cap);
    const auto failed = dir.path / "failed.bmp";
    presenter->ArmScreenshot(std::chrono::steady_clock::now(), failed);
    REQUIRE(presenter->Present());
    CHECK(fake->dispatches == 4);
    CHECK_FALSE(presenter->UpscalerActive());
    CHECK(presenter->NextJitter() == std::pair<float, float>{0.0f, 0.0f});
    CHECK(presenter->MipBias() == 0.0f);
    CHECK(PixelsDifferentIn(ReadAll(failed), 400, 300, 800, 500, 128, 128, 128, true) == 0);
    CHECK(d->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

// Resizes with the upscaler (Alt+Enter): the recreated output texture may have the same pointer
// as the previous one just freed, and its SRV must be rewritten anyway (otherwise the drawing reads
// freed memory: DXGI_ERROR_DEVICE_HUNG in game, with FSR and with DLSS).
TEST_CASE("presenter: upscaler, the output recreated on every resize is drawn", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  plat::Window* window = plat::CreateGameWindow("gow2 presenter upscaler resize", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_presenter_upscaler_resize");
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    constexpr uint32_t kRW = 640, kRH = 360;
    gpu::UpscaleCapture cap{};
    cap.prehud = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
    cap.depth = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    cap.velocity = gow2_test::MakeTexture(d, kRW / 2, kRH / 2, DXGI_FORMAT_R16G16_TYPELESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(presenter->RefreshGuestOutput(kRW, kRH, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
      auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
      const std::vector<uint32_t> gray(kRW * kRH, 0xC0200802u);
      gow2_test::Upload(*provider, d3d12_context.resource_uav_capable(), 4, gray.data(),
                        D3D12Presenter::kGuestOutputInternalState);
      return true;
    }));
    // Fake upscaler that fills the output with green, whatever its size.
    struct Green final : gpu::Upscaler {
      const xe::ui::d3d12::D3D12Provider& provider;
      std::string name = "green";
      explicit Green(const xe::ui::d3d12::D3D12Provider& p) : provider(p) {}
      const std::string& Name() const override { return name; }
      uint32_t JitterPhaseCount(uint32_t, uint32_t) override { return 8; }
      void JitterOffset(uint32_t, uint32_t, float& x, float& y) override { x = y = 0.0f; }
      bool Dispatch(const gpu::UpscaleFrame& f) override {
        auto fill = gow2_test::MakeTexture(provider.GetDevice(), f.output_w, f.output_h,
                                           D3D12Presenter::kGuestOutputFormat, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const std::vector<uint32_t> green(size_t(f.output_w) * f.output_h, 1023u << 10 | 3u << 30);
        gow2_test::Upload(provider, fill.Get(), 4, green.data(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        fills.push_back(fill);  // alive while the GPU may copy it
        gow2_test::Barrier(f.cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        f.cmd->CopyResource(f.output, fill.Get());
        gow2_test::Barrier(f.cmd, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return true;
      }
      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> fills;
    };
    presenter->SetUpscaler([&](uint32_t, uint32_t, uint32_t, uint32_t) { return std::make_unique<Green>(*provider); },
                           -1.0f, gpu::UpscaleDebug::kNone);

    // Window alternating between 1280x720 and 960x540 (16:9: the whole area is upscaler output).
    for (int i = 0; i < 12; ++i) {
      const int w = i % 2 ? 960 : 1280, h = i % 2 ? 540 : 720;
      RECT r{0, 0, w, h};
      AdjustWindowRectEx(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
      SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
      presenter->SetUpscaleCapture(&cap);
      const auto file = dir.path / ("resize" + std::to_string(i) + ".bmp");
      presenter->ArmScreenshot(std::chrono::steady_clock::now(), file);
      REQUIRE(presenter->Present());
      const auto bmp = ReadAll(file);
      REQUIRE(bmp.size() == 54 + size_t(w) * h * 3);
      INFO("resize " << i << " to " << w << "x" << h);
      CHECK(PixelsDifferentIn(bmp, 0, 0, w, h, 0, 255, 0, true) == 0);
      REQUIRE(d->GetDeviceRemovedReason() == S_OK);
    }
  }
  plat::DestroyGameWindow(window);
}

namespace {

// Red 1280x720 guest output in the presenter.
bool RefreshRed(gpu::Presenter& presenter, const xe::ui::d3d12::D3D12Provider& provider) {
  return presenter.RefreshGuestOutput(1280, 720, 16, 9, [&](xe::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    return FillGuestOutput(provider, d3d12_context.resource_uav_capable(), 0x3FFu | (3u << 30));
  });
}

std::vector<uint8_t> Shot(gpu::Presenter& presenter, const std::filesystem::path& file) {
  presenter.ArmScreenshot(std::chrono::steady_clock::now(), file);
  if (!presenter.Present()) return {};
  return ReadAll(file);
}


}  // namespace

TEST_CASE("presenter: F1 opens the overlay over the guest output, F1 closes it again", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 overlay test", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_overlay");
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    auto overlay = gpu::Overlay::Create(window, dir.path / "gow2.toml", {}, {});
    REQUIRE(overlay);
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    REQUIRE(RefreshRed(*presenter, *provider));
    CHECK(PixelsDifferentFrom(Shot(*presenter, dir.path / "closed.bmp"), 255, 0, 0) == 0);

    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    CHECK(ov->IsOpen());
    for (int i = 0; i < 2; ++i) REQUIRE(presenter->Present());
    const auto open = Shot(*presenter, dir.path / "open.bmp");
    REQUIRE(open.size() == 54 + 1280 * 720 * 3);
    CHECK_FALSE(Near(PixelAt(open, 640, 360), {255, 0, 0}));
    CHECK(PixelsDifferentFrom(open, 255, 0, 0) > 1280 * 720 / 5);
    CHECK(Near(PixelAt(open, 5, 5), {255, 0, 0}));  // outside the panel

    SendMessageW(hwnd, WM_KEYUP, VK_F1, 0xC03B0001);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    CHECK_FALSE(ov->IsOpen());
    CHECK(PixelsDifferentFrom(Shot(*presenter, dir.path / "reclosed.bmp"), 255, 0, 0) == 0);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: Enter on the open overlay saves the file and calls on_apply", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 overlay test", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_overlay");
  const auto config = dir.path / "gow2.toml";
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    settings::Settings running;
    running.video.vsync = false;
    running.language = "it";
    running.video.anisotropic = 4;
    int applied = 0;
    settings::Settings got;
    presenter->SetOverlay(gpu::Overlay::Create(window, config, running, [&](const settings::Settings& s) {
      ++applied;
      got = s;
    }));
    REQUIRE(RefreshRed(*presenter, *provider));
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    for (int i = 0; i < 3; ++i) REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
    for (int i = 0; i < 2; ++i) REQUIRE(presenter->Present());
    CHECK(applied == 1);
    CHECK(got == running);
    settings::Settings loaded;
    REQUIRE(std::filesystem::exists(config));
    CHECK_FALSE(settings::LoadFile(config, loaded));
    CHECK(loaded == running);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: SetVideo changes stretch and vsync at the next Present", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window));
    REQUIRE(presenter);
    REQUIRE(presenter->Present());
    gpu::VideoChange change{};
    change.vsync = false;
    change.stretch = true;
    change.sharpening = true;
    change.sharpness = 0.25f;
    presenter->SetVideo(change);
    CHECK(presenter->FrameOptions().vsync);
    CHECK_FALSE(presenter->FrameOptions().stretch);
    REQUIRE(presenter->Present());
    CHECK_FALSE(presenter->FrameOptions().vsync);
    CHECK(presenter->FrameOptions().stretch);
    CHECK(presenter->FrameOptions().sharpening);
    CHECK(presenter->FrameOptions().sharpness == 0.25f);
    REQUIRE(presenter->Present());  // without vsync: Present(0, tearing if supported) succeeds
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame rate 30 does not query the monitor", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    gpu::PresentOptions options;
    options.frame_rate = settings::FrameRate::k30;
    options.vrr = settings::Vrr::kOn;
    int calls = 0;
    options.display_sync = [&calls](bool) { return ++calls, plat::DisplaySync{100, {}, ""}; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    for (int i = 0; i < 10; ++i) REQUIRE(presenter->Present());
    CHECK(calls == 0);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: vrr auto queries the monitor every 500 ms", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  for (const auto vrr : {settings::Vrr::kAuto, settings::Vrr::kOn}) {
    plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
    REQUIRE(window);
    {
      gpu::PresentOptions options;
      options.vsync = false;
      options.frame_rate = settings::FrameRate::kUnlimited;
      options.vrr = vrr;
      int calls = 0;
      options.display_sync = [&calls](bool) { return ++calls, plat::DisplaySync{100, {}, ""}; };
      auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
      REQUIRE(presenter);
      REQUIRE(presenter->Present());  // first Present: creates the resources and queries once
      // t0 after the first Present. Auto: wide deadline (3 s), reaching 3 calls is enough (each ~500 ms after the
      // previous one, with 2 s 4 would be needed). On: 1.5 s without other calls (the period is 2 s).
      const auto t0 = std::chrono::steady_clock::now();
      const auto limit = std::chrono::milliseconds(vrr == settings::Vrr::kAuto ? 3000 : 1500);
      while (std::chrono::steady_clock::now() - t0 < limit && !(vrr == settings::Vrr::kAuto && calls >= 3))
        REQUIRE(presenter->Present());
      // A failure seen once under load did not reproduce again (6 suites): this data will tell whether it is
      // the minimized window (Present exits before PaceFrame) or the timing.
      INFO("vrr " << int(vrr) << ", calls " << calls << ", ms "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
                  << ", client " << plat::ClientSize(window).first << "x" << plat::ClientSize(window).second);
      if (vrr == settings::Vrr::kAuto) CHECK(calls >= 3);
      else CHECK(calls <= 1);
    }
    plat::DestroyGameWindow(window);
  }
}

TEST_CASE("presenter: limiter at the VRR limit, turned off by SetVideo with vrr off", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    gpu::PresentOptions options;
    options.vsync = false;
    options.frame_rate = settings::FrameRate::kUnlimited;
    options.vrr = settings::Vrr::kOn;
    int calls = 0;
    options.display_sync = [&calls](bool) { return ++calls, plat::DisplaySync{100, {}, ""}; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    // 21 Presents = 20 intervals at the VrrCap(100) = 97.22 fps limit.
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 21; ++i) REQUIRE(presenter->Present());
    const double limited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(limited >= 20 / 97.22 * 0.98);
    CHECK(calls >= 1);

    const int before = calls;
    gpu::VideoChange change{};
    change.vsync = false;
    change.vrr = settings::Vrr::kOff;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    CHECK(calls == before + 1);  // queried again right after SetVideo
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 21; ++i) REQUIRE(presenter->Present());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: overlay closed with F1 restarts clean on reopening", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 overlay test", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_overlay");
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    // Nonexistent folder: Apply fails and leaves the error in the panel.
    auto overlay = gpu::Overlay::Create(window, dir.path / "missing" / "gow2.toml", {}, {});
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    REQUIRE(RefreshRed(*presenter, *provider));
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    for (int i = 0; i < 3; ++i) REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
    REQUIRE(presenter->Present());
    REQUIRE(ov->State().save_failed);
    REQUIRE_FALSE(ov->State().error.empty());

    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);  // closes
    REQUIRE_FALSE(ov->IsOpen());
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);  // reopens
    REQUIRE(presenter->Present());
    CHECK_FALSE(ov->State().save_failed);
    CHECK(ov->State().error.empty());
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: Esc with an open combo closes only the combo", "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 overlay test", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_overlay");
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    auto overlay = gpu::Overlay::Create(window, dir.path / "gow2.toml", {}, {});
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    REQUIRE(RefreshRed(*presenter, *provider));
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    for (int i = 0; i < 3; ++i) REQUIRE(presenter->Present());
    // Click on the Mode combo (640x1100 panel at ~0.68 scale in a 1280x720 window, horizontally centered: row at y ~36, controls column x ~744).
    // The real cursor is not on the invisible window: after every WM_MOUSEMOVE a WM_MOUSELEAVE arrives, so
    // every button is preceded by a move in the same frame.
    const LPARAM at = MAKELPARAM(744, 36);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, at);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, at);
    for (int i = 0; i < 2; ++i) REQUIRE(presenter->Present());
    REQUIRE(ov->PopupOpen());

    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    REQUIRE(presenter->Present());
    CHECK_FALSE(ov->PopupOpen());
    CHECK(ov->IsOpen());

    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    REQUIRE(presenter->Present());
    SendMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    CHECK_FALSE(ov->IsOpen());
  }
  plat::DestroyGameWindow(window);
}

// Overlay closed with Esc while dragging (button pressed, capture taken): the release arrives with the overlay closed and
// must still drop the capture, otherwise clicks on the desktop go to the game.
TEST_CASE("presenter: overlay closed during a drag, the release drops the capture", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 overlay test", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
  auto captured = [&] {
    GUITHREADINFO gui{sizeof(gui)};
    return GetGUIThreadInfo(tid, &gui) && gui.hwndCapture == hwnd;
  };
  auto settle = [&](bool want) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (captured() != want && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return captured() == want;
  };
  gow2_test::TempDir dir("gow2_overlay_capture");
  {
    auto presenter = gpu::Presenter::Create(*provider, hwnd);
    REQUIRE(presenter);
    auto overlay = gpu::Overlay::Create(window, dir.path / "gow2.toml", {}, {});
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    REQUIRE(RefreshRed(*presenter, *provider));
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    for (int i = 0; i < 3; ++i) REQUIRE(presenter->Present());
    // The point MUST fall on a widget: on an empty area the click activates ImGui's window move
    // (ActiveID stays while the button is down) and Esc does not close the overlay (busy). 1280x720 window: 640x1100 panel
    // at ~0.68 scale (x 422-858), without the launcher's warning row. At x=640 y=304 it falls on the Sharpness slider
    // (Sharpness row, right below Anti-aliasing and above Internal resolution); REQUIRE(IsAnyItemHovered) checks it.
    // If the layout changes a point on a widget must be found again (not on a checkbox: at x=640 it does not hit it).
    const LPARAM at = MAKELPARAM(640, 304);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, at);
    REQUIRE(presenter->Present());
    REQUIRE(ImGui::IsAnyItemHovered());
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, at);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
    REQUIRE(presenter->Present());
    CHECK(settle(true));
    // The first Esc may only release ImGui's active item: repeat until the overlay closes.
    for (int i = 0; i < 3 && ov->IsOpen(); ++i) {
      SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
      REQUIRE(presenter->Present());
      SendMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001);
      REQUIRE(presenter->Present());
    }
    REQUIRE_FALSE(ov->IsOpen());
    CHECK(captured());  // button still pressed
    SendMessageW(hwnd, WM_LBUTTONUP, 0, at);
    CHECK(settle(false));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: exclusive stays allowed after SetVideo without exclusive (Alt-Enter returns to fullscreen)",
          "[presenter]") {
  const auto adapter = gpu::FindHardwareAdapter();
  if (!adapter) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter test", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    gpu::PresentOptions options;
    // Never really in exclusive: it would change the monitor mode under the other tests.
    options.exclusive_wanted = [](bool) { return false; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    gpu::VideoChange change{};
    change.exclusive_fullscreen = true;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    CHECK(presenter->FrameOptions().exclusive_fullscreen);
    change.exclusive_fullscreen = false;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    CHECK(presenter->FrameOptions().exclusive_fullscreen);
  }
  plat::DestroyGameWindow(window);
}

// ---- Frame generation: fake generator on a normal swap chain ----

namespace {

struct FgCall {
  gpu::FrameGenFrame f;
  bool inputs;
  uint32_t display_w, render_w, hudless_w, hudless_h;
  DXGI_FORMAT hudless_format;
  float hdr_peak_nits;
  D3D12_RESOURCE_FLAGS hudless_flags;
};

struct FgPresent {
  UINT sync_interval, flags, index;
};

// Without own_buffers it forwards GetBuffer, CurrentBufferIndex and Present to the swap chain (like FSR to the proxy); with
// own_buffers it gives its own textures, indices from `indices` and no DXGI Present (like the DLSS generator).
class FakeFrameGenerator final : public gpu::FrameGenerator {
 public:
  explicit FakeFrameGenerator(bool* shut_down) : shut_down_(shut_down) {}
  HRESULT GetBuffer(UINT i, ID3D12Resource** out) override {
    events.push_back("get" + std::to_string(i));
    if (!own_buffers) return swap_chain->GetBuffer(i, IID_PPV_ARGS(out));
    DXGI_SWAP_CHAIN_DESC1 d{};
    swap_chain->GetDesc1(&d);
    if (!own[i] || own[i]->GetDesc().Width != d.Width || own[i]->GetDesc().Height != d.Height)
      own[i] = gow2_test::MakeTexture(device, d.Width, d.Height, d.Format, D3D12_RESOURCE_STATE_PRESENT,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    return own[i].CopyTo(out);
  }
  UINT CurrentBufferIndex() override {
    if (!own_buffers) return swap_chain->GetCurrentBackBufferIndex();
    return indices.empty() ? 0 : indices[presents.size() % indices.size()];
  }
  HRESULT Present(UINT sync_interval, UINT flags) override {
    presents.push_back({sync_interval, flags, CurrentBufferIndex()});
    return own_buffers ? S_OK : swap_chain->Present(sync_interval, flags);
  }
  void Frame(IDXGISwapChain4*, uint32_t display_w, uint32_t, const gpu::FrameGenFrame& f,
             const gpu::FrameGenInputs* in) override {
    FgCall c{f, in != nullptr, display_w};
    if (in) {
      const D3D12_RESOURCE_DESC h = in->hudless->GetDesc();
      c.render_w = in->render_w;
      c.hudless_w = uint32_t(h.Width);
      c.hudless_h = h.Height;
      c.hudless_format = h.Format;
      c.hdr_peak_nits = in->hdr_peak_nits;
      c.hudless_flags = h.Flags;
    }
    calls.push_back(c);
  }
  void WaitForPresents() override {
    ++waits;
    events.push_back("wait");
  }
  void Shutdown(Microsoft::WRL::ComPtr<IDXGISwapChain4>& sc) override {
    sc.Reset();
    *shut_down_ = true;
  }
  std::vector<FgCall> calls;
  int waits = 0;
  std::vector<std::string> events;  // "getI" and "wait" in order
  std::vector<FgPresent> presents;
  IDXGISwapChain4* swap_chain = nullptr;  // the factory's, owned by the presenter
  ID3D12Device* device = nullptr;
  bool own_buffers = false;
  std::vector<UINT> indices;  // CurrentBufferIndex with own_buffers, cyclic over the Presents
  std::array<ComPtr<ID3D12Resource>, 2> own;

 private:
  bool* shut_down_;
};

// Test factory: normal swap chain from the request, fake generator in *fake.
gpu::FrameGeneratorFactory FakeFrameGenFactory(FakeFrameGenerator** fake, bool* shut_down, bool own_buffers = false,
                                               std::vector<UINT> indices = {}) {
  return [=](const gpu::SwapChainRequest& req, ComPtr<IDXGISwapChain4>& out) -> std::unique_ptr<gpu::FrameGenerator> {
    ComPtr<IDXGISwapChain1> sc;
    if (FAILED(req.factory->CreateSwapChainForHwnd(req.queue, req.hwnd, &req.desc, nullptr, nullptr, &sc)) ||
        FAILED(sc.As(&out)))
      return nullptr;
    auto g = std::make_unique<FakeFrameGenerator>(shut_down);
    g->swap_chain = out.Get();
    g->device = req.device;
    g->own_buffers = own_buffers;
    g->indices = indices;
    *fake = g.get();
    return g;
  };
}

std::string ReadLog(const std::filesystem::path& p) {
  const auto b = ReadAll(p);
  return {b.begin(), b.end()};
}

size_t Occurrences(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

}  // namespace

TEST_CASE("presenter: frame generation not available, normal swap chain", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_fg_none");
  LogInit(dir.path / "log.txt");
  {
    gpu::PresentOptions options;
    options.frame_generator = [](const gpu::SwapChainRequest&, ComPtr<IDXGISwapChain4>&) {
      return std::unique_ptr<gpu::FrameGenerator>();
    };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    CHECK(presenter->Present());
    CHECK_FALSE(presenter->FrameGenerationActive());
    CHECK_FALSE(presenter->CaptureWanted());
  }
  LogInit({});
  // The line with the reason is written by the factory (frame_generation.h contract); the test's does not write one.
  CHECK(ReadLog(dir.path / "log.txt").find("[fg] frame generation not available") ==
        std::string::npos);
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, frameID on every Present and inputs only with a capture", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(fake);
    CHECK(presenter->FrameGenerationActive());
    CHECK(presenter->CaptureWanted());  // without upscaler
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);

    REQUIRE(presenter->Present());
    REQUIRE(fake->calls.size() == 1);
    CHECK(fake->calls[0].f.frame_id == 1);
    CHECK_FALSE(fake->calls[0].f.generate);
    CHECK_FALSE(fake->calls[0].f.reset);
    CHECK_FALSE(fake->calls[0].inputs);

    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    REQUIRE(fake->calls.size() == 2);
    const FgCall& c = fake->calls[1];
    CHECK(c.f.frame_id == 2);
    CHECK(c.f.generate);
    CHECK(c.f.reset);
    REQUIRE(c.inputs);
    CHECK(c.render_w == kRW);
    CHECK(c.hudless_w == 1280);
    CHECK(c.hudless_h == 720);
    CHECK(c.hudless_format == DXGI_FORMAT_R10G10B10A2_UNORM);

    REQUIRE(presenter->Present());  // capture consumed
    REQUIRE(fake->calls.size() == 3);
    CHECK(fake->calls[2].f.frame_id == 3);
    CHECK_FALSE(fake->calls[2].f.generate);
    CHECK_FALSE(fake->calls[2].inputs);

    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    REQUIRE(fake->calls.size() == 4);
    CHECK(fake->calls[3].f.frame_id == 4);
    CHECK(fake->calls[3].f.reset);
    CHECK(fake->calls[3].inputs);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, no inputs with the overlay open", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_presenter_fg_overlay");
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    REQUIRE(fake);
    auto overlay = gpu::Overlay::Create(window, dir.path / "gow2.toml", {}, {});
    REQUIRE(overlay);
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    REQUIRE(ov->IsOpen());
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    REQUIRE_FALSE(fake->calls.empty());
    CHECK_FALSE(fake->calls.back().inputs);  // the overlay would vanish from the generated frames
    CHECK_FALSE(fake->calls.back().f.generate);

    SendMessageW(hwnd, WM_KEYUP, VK_F1, 0xC03B0001);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    REQUIRE_FALSE(ov->IsOpen());
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    CHECK(fake->calls.back().inputs);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation turned off hot by SetVideo", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(fake);
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);
    REQUIRE(presenter->Present());

    gpu::VideoChange change{};
    change.frame_generation = false;
    presenter->SetVideo(change);
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    CHECK_FALSE(presenter->CaptureWanted());
    REQUIRE(fake->calls.size() == 2);
    CHECK(fake->calls[1].f.frame_id == 2);
    CHECK_FALSE(fake->calls[1].f.generate);
    CHECK_FALSE(fake->calls[1].inputs);

    change.frame_generation = true;
    presenter->SetVideo(change);
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    CHECK(presenter->CaptureWanted());
    REQUIRE(fake->calls.size() == 3);
    CHECK(fake->calls[2].f.generate);
    CHECK(fake->calls[2].f.reset);
    CHECK(fake->calls[2].inputs);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, WaitForPresents before the resize", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::WindowOptions window_options;
  window_options.resizable = true;
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", window_options, false, [] {});
  REQUIRE(window);
  void* hwnd = plat::NativeHandle(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    REQUIRE(fake);
    REQUIRE(presenter->Present());
    const int waits = fake->waits;
    SetClientSize(hwnd, 960, 540);
    REQUIRE(presenter->Present());
    CHECK(fake->waits >= waits + 1);
    REQUIRE_FALSE(fake->calls.empty());
    CHECK(fake->calls.back().display_w == 960);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, exclusive fullscreen never requested", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_fg_exclusive");
  LogInit(dir.path / "log.txt");
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    int asked = 0;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    options.exclusive_fullscreen = true;
    options.exclusive_wanted = [&](bool) { return ++asked, false; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(presenter->Present());
    gpu::VideoChange change{};
    change.exclusive_fullscreen = true;
    change.frame_generation = true;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    CHECK(asked == 0);
    CHECK_FALSE(presenter->FrameOptions().exclusive_fullscreen);
  }
  LogInit({});
  CHECK(Occurrences(ReadLog(dir.path / "log.txt"),
                    "[fg] exclusive fullscreen not allowed with frame generation: borderless") == 1);
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, real frames at half the proxy's cap", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    options.vsync = false;
    options.frame_rate = settings::FrameRate::kUnlimited;
    options.vrr = settings::Vrr::kOff;
    options.display_sync = [](bool) { return plat::DisplaySync{100, {}, ""}; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    // 11 Presents = 10 intervals at VrrCap(100) / 2 = 48.61 fps, even without VRR, while generating.
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 11; ++i) {
      presenter->SetUpscaleCapture(&cap);
      REQUIRE(presenter->Present());
    }
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= 10 / 48.125 * 0.98);

    gpu::VideoChange change{};
    change.vsync = false;
    change.vrr = settings::Vrr::kOff;
    change.frame_generation = false;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 21; ++i) REQUIRE(presenter->Present());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, full limit in frames without generation", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    options.vsync = false;
    options.frame_rate = settings::FrameRate::kUnlimited;
    options.vrr = settings::Vrr::kOff;
    options.display_sync = [](bool) { return plat::DisplaySync{100, {}, ""}; };
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    // Generation on but no capture (menus, loading): no halved limit, 21 Presents well under
    // the 400 ms of 48 fps.
    REQUIRE(presenter->Present());
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 21; ++i) REQUIRE(presenter->Present());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, Shutdown on destruction", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  FakeFrameGenerator* fake = nullptr;
  bool shut_down = false;
  {
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(presenter->Present());
    CHECK_FALSE(shut_down);
  }
  CHECK(shut_down);
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: frame generation, buffers and Present from the generator", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::WindowOptions window_options;
  window_options.resizable = true;
  plat::Window* window = plat::CreateGameWindow("gow2 presenter fg", window_options, false, [] {});
  REQUIRE(window);
  void* hwnd = plat::NativeHandle(window);
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down, true, {1, 0});
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    REQUIRE(fake);
    CHECK(fake->events == std::vector<std::string>{"get0", "get1"});
    REQUIRE(fake->own[0]);
    REQUIRE(fake->own[1]);
    // Center pixel (R10G10B10A2) of the fake's buffer i; 0 = never drawn (resource created zeroed).
    const auto center = [&](int i) {
      const D3D12_RESOURCE_DESC d = fake->own[i]->GetDesc();
      const auto px = gow2_test::Readback(*provider, fake->own[i].Get(), 4, D3D12_RESOURCE_STATE_PRESENT);
      uint32_t t = 0;
      std::memcpy(&t, px.data() + (size_t(d.Height / 2) * d.Width + d.Width / 2) * 4, 4);
      return t & 0x3FFFFFFFu;  // without alpha
    };
    REQUIRE(RefreshMid(*presenter, *provider));
    REQUIRE(presenter->Present());
    REQUIRE(fake->presents.size() == 1);
    CHECK(fake->presents[0].sync_interval == 1);  // vsync
    CHECK(fake->presents[0].index == 1);
    CHECK(center(1) != 0);
    CHECK(center(0) == 0);
    REQUIRE(presenter->Present());
    REQUIRE(fake->presents.size() == 2);
    CHECK(fake->presents[1].index == 0);
    CHECK(center(0) != 0);

    gpu::VideoChange change{};
    change.vsync = false;
    change.frame_generation = true;
    presenter->SetVideo(change);
    REQUIRE(presenter->Present());
    REQUIRE(fake->presents.size() == 3);
    CHECK(fake->presents[2].sync_interval == 0);

    // Resize: buffers requested again, after WaitForPresents.
    fake->events.clear();
    SetClientSize(hwnd, 960, 540);
    REQUIRE(presenter->Present());
    CHECK(fake->events == std::vector<std::string>{"wait", "get0", "get1"});
    CHECK(fake->own[0]->GetDesc().Width == 960);
    CHECK(fake->presents.size() == 4);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

// ---- HDR10 output ----

namespace {

constexpr hdr::Levels kHdrLevels{1000, 200};

// HDR10 color space accepted and passes created: otherwise the case does SKIP.
bool HdrSupported(const gpu::Presenter& presenter) { return presenter.HdrActive(); }
#define SKIP_WITHOUT_HDR(p) \
  if (!HdrSupported(*(p))) SKIP("HDR10 color space not accepted on this monitor")

struct Ppm {
  uint32_t w = 0, h = 0;
  std::vector<uint16_t> v;  // r, g, b per pixel, from the top
  uint16_t At(uint32_t x, uint32_t y, int c) const { return v[(size_t(y) * w + x) * 3 + c]; }
};

// 16-bit P6 PPM from gpu::WritePpm10 ("P6\n<w> <h>\n1023\n", big-endian).
Ppm ReadPpm10(const std::filesystem::path& path) {
  const auto b = ReadAll(path);
  Ppm p;
  size_t at = 0;
  const auto token = [&] {
    std::string t;
    while (at < b.size() && b[at] != ' ' && b[at] != '\n') t += char(b[at++]);
    ++at;
    return t;
  };
  if (token() != "P6") return {};
  p.w = uint32_t(std::stoul(token()));
  p.h = uint32_t(std::stoul(token()));
  if (token() != "1023" || b.size() - at != size_t(p.w) * p.h * 6) return {};
  p.v.resize(size_t(p.w) * p.h * 3);
  for (size_t i = 0; i < p.v.size(); ++i) p.v[i] = uint16_t(b[at + 2 * i] << 8 | b[at + 2 * i + 1]);
  return p;
}

// 10-bit PQ code of the 10-bit gray gray10 without residual (paper white 200 nit).
int PqCode(uint32_t gray10) {
  const float v = (gray10 & 1023) / 1023.0f;
  return int(std::lround(1023.0f * hdr::ComposePq({v, v, v}, {0, 0, 0}, kHdrLevels.paper_white_nits).r));
}

// PPM channels farther than tol from code in the rectangle [x0, x1) x [y0, y1).
size_t CodesFarFrom(const Ppm& p, int code, int tol, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  size_t count = 0;
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x)
      for (int c = 0; c < 3; ++c) count += std::abs(int(p.At(x, y, c)) - code) > tol;
  return count;
}

// Constants of the "simple path" of tests/gpu/hdr_passes_test.cpp: all taps on the pixel, pre = tf1.
gpu::HdrUberConstants SimpleHdrConstants() {
  gpu::HdrUberConstants k{};
  auto set = [](float* d, float x, float y, float z, float w) {
    d[0] = x;
    d[1] = y;
    d[2] = z;
    d[3] = w;
  };
  set(k.c[0], 1, 0, 0, 0);
  set(k.c[2], 0, 0, 0, -1);
  set(k.c[3], 300, 1e30f, 4, 0);
  set(k.c[5], 0, 0, 0, 1);
  set(k.c[6], 1, 1, 1, 0);
  set(k.c[7], 1, 1, 1, 0);
  set(k.c[9], 1, 1, 1, 0.4545f);
  set(k.c[11], 0, 0, 1, 1);
  set(k.c[12], 24, -2, 0, 0);
  set(k.c[13], 1, 0, 0, 0);
  set(k.c[14], 0, 1, 0, 0);
  set(k.c[15], 0, 0, 0, 1);
  set(k.c[17], 1, 1, 1, 1);
  set(k.c253, 4, 65504, 0, 0);
  set(k.c254, 2, 14, 1e-4f, 1);
  set(k.c255, 0.2f, 0.4f, 0.001f, -1);
  for (int i = 0; i < 3; ++i) k.exp_scale[i] = 1;
  return k;
}

}  // namespace

TEST_CASE("presenter: HDR off, no extra capture", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_hdr_off");
  {
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), gpu::PresentOptions{});
    REQUIRE(presenter);
    CHECK_FALSE(presenter->HdrActive());
    CHECK_FALSE(presenter->CaptureWanted());
    REQUIRE(RefreshMid(*presenter, *provider));
    CHECK(Shoot(*presenter, dir.path / "s.bmp").size() == 54 + 1280 * 720 * 3);
    CHECK_FALSE(std::filesystem::exists(dir.path / "s.pq.ppm"));
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10, SDR and PQ screenshot", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_hdr");
  const uint32_t gray = Gray10(0.5f);
  const auto gray_at = [gray](uint32_t) { return gray; };
  std::vector<uint8_t> sdr_bmp;
  {
    gpu::PresentOptions options;
    options.width = 1280;
    options.height = 720;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, gray_at));
    sdr_bmp = Shoot(*presenter, dir.path / "sdr.bmp");
  }
  {
    gpu::PresentOptions options;
    options.width = 1280;
    options.height = 720;
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    SKIP_WITHOUT_HDR(presenter);
    CHECK(presenter->CaptureWanted());
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, gray_at));
    const auto bmp = Shoot(*presenter, dir.path / "hdr.bmp");
    REQUIRE(bmp.size() == 54 + 1280 * 720 * 3);
    const uint8_t g8 = uint8_t(Gray8(0.5f));
    CHECK(PixelsDifferentFrom(bmp, g8, g8, g8) == 0);
    CHECK(bmp == sdr_bmp);
    const Ppm pq = ReadPpm10(dir.path / "hdr.pq.ppm");
    REQUIRE(pq.w == 1280);
    REQUIRE(pq.h == 720);
    CHECK(CodesFarFrom(pq, PqCode(gray), 1, 0, 0, 1280, 720) == 0);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10, fit with bands", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_hdr_fit");
  {
    gpu::PresentOptions options;
    options.width = 1920;
    options.height = 1080;
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    SKIP_WITHOUT_HDR(presenter);
    const uint32_t gray = Gray10(0.5f);
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 960, [gray](uint32_t) { return gray; }));
    REQUIRE(Shoot(*presenter, dir.path / "fit.bmp").size() == 54 + 1920 * 1080 * 3);
    const Ppm pq = ReadPpm10(dir.path / "fit.pq.ppm");
    REQUIRE(pq.w == 1920);
    REQUIRE(pq.h == 1080);
    // 4:3 in 16:9: 1440x1080 image at x = 240, black bands = PQ of 0 nit.
    CHECK(CodesFarFrom(pq, 0, 1, 0, 0, 240, 1080) == 0);
    CHECK(CodesFarFrom(pq, 0, 1, 1680, 0, 1920, 1080) == 0);
    CHECK(CodesFarFrom(pq, PqCode(gray), 1, 241, 1, 1679, 1079) == 0);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10 with frame generation", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr fg", {1280, 720}, false, [] {});
  REQUIRE(window);
  {  // without HDR: SDR HUD-less (UAV only), peak 0
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(fake);
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    REQUIRE(fake->calls.size() == 1);
    REQUIRE(fake->calls[0].inputs);
    CHECK(fake->calls[0].hdr_peak_nits == 0.0f);
    CHECK(fake->calls[0].hudless_flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  }
  {
    FakeFrameGenerator* fake = nullptr;
    bool shut_down = false;
    gpu::PresentOptions options;
    options.frame_generator = FakeFrameGenFactory(&fake, &shut_down);
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    REQUIRE(fake);
    SKIP_WITHOUT_HDR(presenter);
    const gpu::UpscaleCapture cap = MakeCapture(*presenter, *provider);  // empty scene and bloom: no residual
    presenter->SetUpscaleCapture(&cap);
    REQUIRE(presenter->Present());
    REQUIRE(fake->calls.size() == 1);
    const FgCall& c = fake->calls[0];
    CHECK(c.f.generate);
    REQUIRE(c.inputs);
    CHECK(c.hdr_peak_nits == 1000.0f);
    CHECK(c.hudless_w == 1280);
    CHECK(c.hudless_h == 720);
    CHECK(c.hudless_format == DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK((c.hudless_flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10, residual only from the frame with the scene", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* d = provider->GetDevice();
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("gow2_presenter_hdr_resid");
  {
    gpu::PresentOptions options;
    options.width = 1280;
    options.height = 720;
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, plat::NativeHandle(window), options);
    REQUIRE(presenter);
    SKIP_WITHOUT_HDR(presenter);

    // tf1 = (4, 4, 4): the SDR saturates (prehud 255, white guest output = ramp(prehud), HUD mask 0) and
    // the extra light goes into the residual.
    constexpr auto kCopy = D3D12_RESOURCE_STATE_COPY_DEST;
    gpu::UpscaleCapture cap{};
    cap.prehud = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R8G8B8A8_UNORM, kCopy);
    cap.depth = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R32_FLOAT, kCopy);
    cap.velocity = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R16G16_TYPELESS, kCopy);
    cap.scene = gow2_test::MakeTexture(d, kRW, kRH, DXGI_FORMAT_R16G16B16A16_FLOAT, kCopy);
    cap.bloom = gow2_test::MakeTexture(d, 4, 4, DXGI_FORMAT_R16G16B16A16_FLOAT, kCopy);
    const std::vector<uint32_t> white(kRW * kRH, 0xFFFFFFFFu), zeros(kRW * kRH * 2, 0);
    const std::vector<float> depth(kRW * kRH, 0.5f);
    std::vector<uint16_t> scene(kRW * kRH * 4, 0x4400);  // half 4.0
    for (size_t i = 3; i < scene.size(); i += 4) scene[i] = 0;
    gow2_test::Upload(*provider, cap.prehud.Get(), 4, white.data(), kCopy);
    gow2_test::Upload(*provider, cap.depth.Get(), 4, depth.data(), kCopy);
    gow2_test::Upload(*provider, cap.velocity.Get(), 4, zeros.data(), kCopy);
    gow2_test::Upload(*provider, cap.scene.Get(), 8, scene.data(), kCopy);
    gow2_test::Upload(*provider, cap.bloom.Get(), 8, zeros.data(), kCopy);
    cap.hdr = SimpleHdrConstants();
    cap.ramp.pwl = false;
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = uint32_t(std::lround(i * 1023.0 / 255.0));
      cap.ramp.table[i] = v | v << 10 | v << 20;
    }
    const uint32_t guest = Gray10(1.0f);
    REQUIRE(RefreshWith(*presenter, *provider, kRW, kRH, [guest](uint32_t) { return guest; }));
    const int base = PqCode(guest);

    presenter->SetUpscaleCapture(&cap);
    REQUIRE(Shoot(*presenter, dir.path / "scene.bmp").size() == 54 + 1280 * 720 * 3);
    const Ppm lit = ReadPpm10(dir.path / "scene.pq.ppm");
    REQUIRE(lit.w == 1280);
    for (int c = 0; c < 3; ++c) CHECK(int(lit.At(640, 360, c)) > base + 5);

    REQUIRE(Shoot(*presenter, dir.path / "plain.bmp").size() == 54 + 1280 * 720 * 3);  // capture consumed
    const Ppm plain = ReadPpm10(dir.path / "plain.pq.ppm");
    REQUIRE(plain.w == 1280);
    CHECK(CodesFarFrom(plain, base, 1, 0, 0, 1280, 720) == 0);
    CHECK(d->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10 after the window resize", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::WindowOptions window_options;
  window_options.resizable = true;
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr resize", window_options, false, [] {});
  REQUIRE(window);
  void* hwnd = plat::NativeHandle(window);
  gow2_test::TempDir dir("gow2_presenter_hdr_resize");
  {
    gpu::PresentOptions options;
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    SKIP_WITHOUT_HDR(presenter);
    const uint32_t gray = Gray10(0.5f);
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, [gray](uint32_t) { return gray; }));
    REQUIRE(presenter->Present());
    SetClientSize(hwnd, 1600, 900);
    REQUIRE(Shoot(*presenter, dir.path / "1600x900.bmp").size() == 54 + 1600 * 900 * 3);
    const Ppm pq = ReadPpm10(dir.path / "1600x900.pq.ppm");
    REQUIRE(pq.w == 1600);
    REQUIRE(pq.h == 900);
    CHECK(CodesFarFrom(pq, PqCode(gray), 1, 0, 0, 1600, 900) == 0);  // 16:9 in 16:9: fit = the whole area
    CHECK(presenter->HdrActive());
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}

TEST_CASE("presenter: HDR10 with the overlay open", "[presenter]") {
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 presenter hdr overlay", {1280, 720}, false, [] {});
  REQUIRE(window);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(window));
  gow2_test::TempDir dir("gow2_presenter_hdr_overlay");
  {
    gpu::PresentOptions options;
    options.hdr = kHdrLevels;
    auto presenter = gpu::Presenter::Create(*provider, hwnd, options);
    REQUIRE(presenter);
    SKIP_WITHOUT_HDR(presenter);
    auto overlay = gpu::Overlay::Create(window, dir.path / "gow2.toml", {}, {});
    REQUIRE(overlay);
    const gpu::Overlay* ov = overlay.get();
    presenter->SetOverlay(std::move(overlay));
    const uint32_t gray = Gray10(0.5f);
    const int code = PqCode(gray);
    REQUIRE(RefreshWith(*presenter, *provider, 1280, 720, [gray](uint32_t) { return gray; }));

    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    REQUIRE(ov->IsOpen());
    for (int i = 0; i < 2; ++i) REQUIRE(presenter->Present());
    REQUIRE(Shoot(*presenter, dir.path / "open.bmp").size() == 54 + 1280 * 720 * 3);
    const Ppm open = ReadPpm10(dir.path / "open.pq.ppm");
    REQUIRE(open.w == 1280);
    CHECK(CodesFarFrom(open, code, 1, 0, 0, 10, 10) == 0);  // outside the panel
    CHECK(CodesFarFrom(open, code, 1, 600, 340, 680, 380) > 0);  // inside the panel

    SendMessageW(hwnd, WM_KEYUP, VK_F1, 0xC03B0001);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
    REQUIRE_FALSE(ov->IsOpen());
    REQUIRE(Shoot(*presenter, dir.path / "reclosed.bmp").size() == 54 + 1280 * 720 * 3);
    const Ppm closed = ReadPpm10(dir.path / "reclosed.pq.ppm");
    REQUIRE(closed.w == 1280);
    CHECK(CodesFarFrom(closed, code, 1, 0, 0, 1280, 720) == 0);
    CHECK(provider->GetDevice()->GetDeviceRemovedReason() == S_OK);
  }
  plat::DestroyGameWindow(window);
}
