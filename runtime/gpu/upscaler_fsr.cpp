// See upscaler.h. FSR through the FidelityFX API (SDK v2.3.0, thirdparty/fidelityfx): the
// amd_fidelityfx_loader_dx12.dll loader is loaded at runtime (no import lib) and picks the FSR version
// for the GPU (FSR 3.1.x on NVIDIA, FSR 4 on Radeon RX 7000/9000).
#ifndef _WINDOWS
#define _WINDOWS  // ffx_api_loader.h declares ffxLoadFunctions only with _WINDOWS or PLATFORM_WINDOWS
#endif
#include "gpu/upscaler.h"

#include <windows.h>

#include <string>

#include "dx12/ffx_api_dx12.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "log.h"

namespace gpu {

namespace {

constexpr wchar_t kLoaderDll[] = L"amd_fidelityfx_loader_dx12.dll";

std::string Narrow(const wchar_t* w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

void OnMessage(uint32_t /*type*/, const wchar_t* message) { LogLine("[gpu] FSR: " + Narrow(message)); }

class FsrUpscaler final : public Upscaler {
 public:
  FsrUpscaler(HMODULE dll, const ffxFunctions& ffx, ffxContext context, std::string name)
      : dll_(dll), ffx_(ffx), context_(context), name_(std::move(name)) {}
  ~FsrUpscaler() override {
    ffx_.DestroyContext(&context_, nullptr);
    FreeLibrary(dll_);
  }

  const std::string& Name() const override { return name_; }

  uint32_t JitterPhaseCount(uint32_t render_w, uint32_t output_w) override {
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = render_w;
    q.displayWidth = output_w;
    q.pOutPhaseCount = &phases;
    return ffx_.Query(&context_, &q.header) == FFX_API_RETURN_OK && phases > 0 ? uint32_t(phases) : 1;
  }

  void JitterOffset(uint32_t index, uint32_t phases, float& x, float& y) override {
    x = y = 0.0f;
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = int32_t(index % (phases ? phases : 1));
    q.phaseCount = int32_t(phases);
    q.pOutX = &x;
    q.pOutY = &y;
    ffx_.Query(&context_, &q.header);
  }

  bool Dispatch(const UpscaleFrame& f) override {
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = f.cmd;
    // Inputs in the state the presenter leaves them in (NON_PIXEL | PIXEL_SHADER_RESOURCE).
    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    d.color = ffxApiGetResourceDX12(f.color, kRead);
    d.depth = ffxApiGetResourceDX12(f.depth, kRead);
    d.motionVectors = ffxApiGetResourceDX12(f.motion, kRead);
    if (f.reactive) d.reactive = ffxApiGetResourceDX12(f.reactive, kRead);
    d.output = ffxApiGetResourceDX12(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter_x, f.jitter_y};
    d.motionVectorScale = {f.mv_scale_x, f.mv_scale_y};
    d.renderSize = {f.render_w, f.render_h};
    d.upscaleSize = {f.output_w, f.output_h};
    d.enableSharpening = f.sharpness >= 0.0f;
    d.sharpness = f.sharpness >= 0.0f ? f.sharpness : 0.0f;
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    // UE3: near 10 units, infinite far; FOV not exposed by the game.
    // shortcut: fixed vertical FOV of 1 rad, upgrade: derive it from the projection in the vertex shader constants.
    d.cameraNear = 10.0f;
    d.cameraFar = 1e6f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 0.02f;  // UE3 units ~ 2 cm
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (f.debug_view) d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    const ffxReturnCode_t rc = ffx_.Dispatch(&context_, &d.header);
    if (rc != FFX_API_RETURN_OK) {
      LogLine("[gpu] FSR: dispatch failed (code " + std::to_string(rc) + ")");
      return false;
    }
    return true;
  }

 private:
  HMODULE dll_;
  ffxFunctions ffx_;
  ffxContext context_;
  std::string name_;
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsrUpscaler(ID3D12Device* device, uint32_t max_render_w, uint32_t max_render_h,
                                            uint32_t max_output_w, uint32_t max_output_h,
                                            const std::filesystem::path& dll_dir) {
  const std::filesystem::path path = dll_dir / kLoaderDll;
  HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) {
    LogLine("[gpu] FSR unavailable: " + path.string() + " could not be loaded (error " + std::to_string(GetLastError()) +
            ")");
    return nullptr;
  }
  ffxFunctions ffx{};
  ffxLoadFunctions(&ffx, dll);
  if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Query || !ffx.Dispatch || !device) {
    LogLine(device ? "[gpu] FSR unavailable: missing functions in the loader" : "[gpu] FSR unavailable: no device");
    FreeLibrary(dll);
    return nullptr;
  }

  ffxCreateContextDescUpscale create{};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  // DEBUG_VISUALIZATION: enables the debug view (gow2_upscaler_debug=ffx_debug_view), a little more memory.
  create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
  create.maxRenderSize = {max_render_w, max_render_h};
  create.maxUpscaleSize = {max_output_w, max_output_h};
  create.fpMessage = OnMessage;
  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device;
  ffxCreateContextDescUpscaleVersion version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.version = FFX_UPSCALER_VERSION;
  create.header.pNext = &backend.header;
  backend.header.pNext = &version.header;

  ffxContext context = nullptr;
  const ffxReturnCode_t rc = ffx.CreateContext(&context, &create.header, nullptr);
  if (rc != FFX_API_RETURN_OK) {
    LogLine("[gpu] FSR unavailable: context creation failed (code " + std::to_string(rc) + ")");
    FreeLibrary(dll);
    return nullptr;
  }
  ffxQueryGetProviderVersion v{};
  v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx.Query(&context, &v.header);
  const std::string name = std::string("FSR ") + (v.versionName ? v.versionName : "?");
  return std::make_unique<FsrUpscaler>(dll, ffx, context, name);
}

}  // namespace gpu
