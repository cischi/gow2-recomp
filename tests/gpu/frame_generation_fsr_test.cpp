// Frame generation FSR generator (runtime/gpu/frame_generation.h): FidelityFX proxy swap chain
// loaded at runtime from the given folder. The case with the DLLs is skipped (SKIP) if the build does not have them
// (cmake/FidelityFX.cmake) or without a hardware D3D12 adapter.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gpu/d3d12_probe.h"
#include "gpu/frame_generation.h"
#include "log.h"
#include "platform/platform.h"
#include "temp_dir.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace {

using Microsoft::WRL::ComPtr;

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("frame generation: not available without DLLs", "[frame_generation]") {
  gow2_test::TempDir dir("frame_generation");
  LogInit(dir.path / "log.txt");
  ComPtr<IDXGISwapChain4> out;
  CHECK_FALSE(gpu::CreateFsrFrameGenerator(gpu::SwapChainRequest{}, out, dir.path));
  CHECK_FALSE(out);
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  CHECK(log.find("[fg] frame generation not available (loader missing): normal swap chain") != std::string::npos);
}

TEST_CASE("frame generation: proxy created and closed with the DLLs", "[frame_generation]") {
  if (!std::filesystem::exists(ExeDir() / "amd_fidelityfx_framegeneration_dx12.dll"))
    SKIP("frame generation DLL not in the build");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 frame generation test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("frame_generation");
  LogInit(dir.path / "log.txt");
  {
    gpu::SwapChainRequest req{};
    req.device = provider->GetDevice();
    req.factory = provider->GetDXGIFactory();
    req.queue = provider->GetDirectQueue();
    req.hwnd = static_cast<HWND>(plat::NativeHandle(window));
    // Like Presenter::Initialize.
    req.desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    req.desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    req.desc.SampleDesc.Count = 1;
    req.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    req.desc.BufferCount = 2;
    req.desc.Scaling = DXGI_SCALING_STRETCH;
    req.desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    req.desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateFsrFrameGenerator(req, out, ExeDir());
    REQUIRE(fg);
    REQUIRE(out);
    for (uint64_t i = 1; i <= 3; ++i) {
      fg->Frame(out.Get(), 1280, 720, gpu::FrameGenFrame{i, false, false, 16.7f}, nullptr);
      CHECK(out->Present(0, 0) == S_OK);
    }
    fg->Shutdown(out);
    CHECK_FALSE(out);
  }
  LogInit({});
  plat::DestroyGameWindow(window);
  const std::string log = ReadText(dir.path / "log.txt");
  CHECK(log.find("[fg] swap chain proxy ") != std::string::npos);
  CHECK(log.find(" created in ") != std::string::npos);
  CHECK(log.find("[fg] closing: real ") != std::string::npos);
}

namespace {

ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r;
  dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               nullptr, IID_PPV_ARGS(&r));
  return r;
}

}  // namespace

TEST_CASE("frame generation FSR: HDR context", "[frame_generation]") {
  if (!std::filesystem::exists(ExeDir() / "amd_fidelityfx_framegeneration_dx12.dll"))
    SKIP("frame generation DLL not in the build");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  plat::Window* window = plat::CreateGameWindow("gow2 frame generation test", {1280, 720}, false, [] {});
  REQUIRE(window);
  gow2_test::TempDir dir("frame_generation");
  LogInit(dir.path / "log.txt");
  {
    ID3D12Device* dev = provider->GetDevice();
    gpu::SwapChainRequest req{};
    req.device = dev;
    req.factory = provider->GetDXGIFactory();
    req.queue = provider->GetDirectQueue();
    req.hwnd = static_cast<HWND>(plat::NativeHandle(window));
    req.desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    req.desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    req.desc.SampleDesc.Count = 1;
    req.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    req.desc.BufferCount = 2;
    req.desc.Scaling = DXGI_SCALING_STRETCH;
    req.desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    req.desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain4> out;
    auto fg = gpu::CreateFsrFrameGenerator(req, out, ExeDir());
    REQUIRE(fg);
    REQUIRE(out);
    auto depth = MakeTex(dev, DXGI_FORMAT_R32_FLOAT, 1280, 720);
    auto motion = MakeTex(dev, DXGI_FORMAT_R16G16_FLOAT, 1280, 720);
    auto hudless = MakeTex(dev, DXGI_FORMAT_R10G10B10A2_UNORM, 1280, 720);
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))));
    cmd->Close();
    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t fv = 0;
    for (uint64_t i = 1; i <= 2; ++i) {
      const float peak = i == 1 ? 1000.0f : 0.0f;  // the second frame goes back to SDR: context recreated
      alloc->Reset();
      cmd->Reset(alloc.Get(), nullptr);
      gpu::FrameGenInputs in{cmd.Get(), depth.Get(), motion.Get(), hudless.Get(), 1280, 720, 0, 0, peak};
      fg->Frame(out.Get(), 1280, 720, gpu::FrameGenFrame{i, true, i == 1, 16.7f}, &in);
      {  // the proxy wants the back buffer touched by the game, otherwise the Present is an invalid call
        ComPtr<ID3D12Resource> bb;
        REQUIRE(fg->CurrentBufferIndex() == out->GetCurrentBackBufferIndex());  // forwarded to the proxy
        REQUIRE(SUCCEEDED(fg->GetBuffer(fg->CurrentBufferIndex(), &bb)));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1, &b);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
      }
      cmd->Close();
      ID3D12CommandList* lists[] = {cmd.Get()};
      req.queue->ExecuteCommandLists(1, lists);
      req.queue->Signal(fence.Get(), ++fv);
      if (fence->GetCompletedValue() < fv) {
        fence->SetEventOnCompletion(fv, nullptr);  // blocks until completion
      }
      CHECK(fg->Present(0, 0) == S_OK);
    }
    fg->Shutdown(out);
  }
  LogInit({});
  plat::DestroyGameWindow(window);
  const std::string log = ReadText(dir.path / "log.txt");
  CHECK(log.find("[fg] ffx error") == std::string::npos);
  const size_t first = log.find("[fg] FG context ");
  REQUIRE(first != std::string::npos);
  const size_t first_end = log.find('\n', first);
  CHECK(log.substr(first, first_end - first).ends_with(", HDR"));
  const size_t second = log.find("[fg] FG context ", first_end);
  REQUIRE(second != std::string::npos);
  const std::string l2 = log.substr(second, log.find('\n', second) - second);
  CHECK(l2.find("HDR") == std::string::npos);
}
