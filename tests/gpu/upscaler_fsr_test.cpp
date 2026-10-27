// gpu::Upscaler interface (runtime/gpu/upscaler.h): FSR adapter (FidelityFX API, DLLs loaded at
// runtime from the given folder). The case with the DLLs is skipped (SKIP) if the build
// does not have them (cmake/FidelityFX.cmake) or without a hardware D3D12 adapter.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gpu/d3d12_probe.h"
#include "gpu/upscaler.h"
#include "log.h"
#include "temp_dir.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

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

ComPtr<ID3D12Resource> Texture(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav,
                               D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = f;
  desc.SampleDesc.Count = 1;
  desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ComPtr<ID3D12Resource> r;
  REQUIRE(SUCCEEDED(d->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                               &desc, state, nullptr, IID_PPV_ARGS(&r))));
  return r;
}

}  // namespace

TEST_CASE("upscaler: FSR not available without DLLs", "[upscaler]") {
  gow2_test::TempDir dir("upscaler");
  LogInit(dir.path / "log.txt");
  CHECK_FALSE(gpu::CreateFsrUpscaler(nullptr, 1280, 720, 2560, 1440, dir.path));
  LogInit({});
  const std::string log = ReadText(dir.path / "log.txt");
  CHECK(log.find("amd_fidelityfx_loader_dx12.dll") != std::string::npos);
}

TEST_CASE("upscaler: FSR with the build's DLLs", "[upscaler]") {
  if (!std::filesystem::exists(ExeDir() / "amd_fidelityfx_loader_dx12.dll")) SKIP("FidelityFX DLLs not in the build");
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  auto provider = xe::ui::d3d12::D3D12Provider::Create();
  REQUIRE(provider);
  ID3D12Device* device = provider->GetDevice();

  auto fsr = gpu::CreateFsrUpscaler(device, 1280, 720, 2560, 1440, ExeDir());
  REQUIRE(fsr);
  CHECK(fsr->Name().rfind("FSR ", 0) == 0);
  const uint32_t phases = fsr->JitterPhaseCount(1280, 2560);
  CHECK(phases >= 8);
  for (uint32_t i = 0; i < phases; ++i) {
    float x = 9, y = 9;
    fsr->JitterOffset(i, phases, x, y);
    CHECK(x >= -0.5f);
    CHECK(x <= 0.5f);
    CHECK(y >= -0.5f);
    CHECK(y <= 0.5f);
  }

  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  auto color = Texture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, false, kRead);
  auto depth = Texture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT, false, kRead);
  auto motion = Texture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, false, kRead);
  auto output = Texture(device, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> cmd;
  REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
  REQUIRE(SUCCEEDED(
      device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))));
  for (int frame = 0; frame < 3; ++frame) {
    gpu::UpscaleFrame f{};
    f.cmd = cmd.Get();
    f.color = color.Get();
    f.depth = depth.Get();
    f.motion = motion.Get();
    f.output = output.Get();
    f.render_w = 1280;
    f.render_h = 720;
    f.output_w = 2560;
    f.output_h = 1440;
    fsr->JitterOffset(uint32_t(frame), phases, f.jitter_x, f.jitter_y);
    f.mv_scale_x = f.mv_scale_y = 1.0f;
    f.reset = frame == 0;
    f.sharpness = 0.5f;
    f.frame_ms = 33.3f;
    CHECK(fsr->Dispatch(f));
  }
  REQUIRE(SUCCEEDED(cmd->Close()));
  ID3D12CommandList* lists[] = {cmd.Get()};
  provider->GetDirectQueue()->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
  REQUIRE(SUCCEEDED(provider->GetDirectQueue()->Signal(fence.Get(), 1)));
  REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
  CHECK(device->GetDeviceRemovedReason() == S_OK);
  fsr.reset();  // context destroyed with the GPU idle
}
