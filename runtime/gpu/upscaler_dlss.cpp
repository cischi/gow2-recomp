// See upscaler.h and upscaler_dlss.h. DLSS lives in the separate bridge gow2_dlss.dll (MIT, runtime/gpu/dlss_bridge/:
// the NVIDIA license is incompatible with GPL-3 in the same binary, like bbport's bbport_dlss.dll): here
// only loading, jitter and forwarding of frames through the bridge's C ABI.
#include "gpu/upscaler_dlss.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

#include "gpu/dlss_bridge/gow2_dlss_bridge.h"
#include "gpu/dlss_bridge_loader.h"
#include "gpu/upscaler.h"
#include "log.h"

namespace gpu {

namespace {

float Halton(uint32_t i, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  return r;
}

void OnBridgeLog(const char* line) { LogLine(std::string("[gpu] DLSS: ") + (line ? line : "")); }

class DlssUpscaler final : public Upscaler {
 public:
  DlssUpscaler(HMODULE dll, const Gow2DlssApi* api, void* ctx) : dll_(dll), api_(api), ctx_(ctx), name_(api->name) {}
  ~DlssUpscaler() override {
    api_->Destroy(ctx_);
    FreeLibrary(dll_);
  }

  const std::string& Name() const override { return name_; }
  uint32_t JitterPhaseCount(uint32_t render_w, uint32_t output_w) override {
    return DlssJitterPhaseCount(render_w, output_w);
  }
  void JitterOffset(uint32_t index, uint32_t phases, float& x, float& y) override {
    DlssJitterOffset(index, phases, x, y);
  }

  bool Dispatch(const UpscaleFrame& f) override {
    // One line per process: the context is recreated on every resize.
    static std::atomic<bool> sharpness_logged{false};
    if (f.sharpness >= 0.0f && !sharpness_logged.exchange(true)) LogLine("[gpu] DLSS: sharpness not supported, ignored");
    const Gow2DlssFrame d{f.cmd,      f.color,    f.depth,      f.motion,     f.output,     f.reactive,
                          f.render_w, f.render_h, f.output_w,   f.output_h,   f.jitter_x,   f.jitter_y,
                          f.mv_scale_x, f.mv_scale_y, f.frame_ms, f.reset ? 1 : 0};
    return api_->Evaluate(ctx_, &d) == 0;
  }

 private:
  HMODULE dll_;
  const Gow2DlssApi* api_;
  void* ctx_;
  std::string name_;
};

}  // namespace

uint32_t DlssJitterPhaseCount(uint32_t render_w, uint32_t output_w) {
  if (!render_w) return 8;
  const double ratio = double(output_w) / double(render_w);
  const auto n = uint32_t(std::ceil(8.0 * ratio * ratio - 1e-9));
  return n < 8 ? 8 : n;
}

void DlssJitterOffset(uint32_t index, uint32_t phases, float& x, float& y) {
  const uint32_t i = index % (phases ? phases : 1) + 1;
  x = Halton(i, 2) - 0.5f;
  y = Halton(i, 3) - 0.5f;
}

uint32_t NgxPreset(settings::DlssPreset preset) {
  switch (preset) {
    case settings::DlssPreset::kE: return 5;
    case settings::DlssPreset::kF: return 6;
    case settings::DlssPreset::kJ: return 10;
    case settings::DlssPreset::kK: return 11;
    case settings::DlssPreset::kM: return 13;
    case settings::DlssPreset::kL: return 12;
    case settings::DlssPreset::kDefault: break;
  }
  return 0;
}

std::unique_ptr<Upscaler> CreateDlssUpscaler(ID3D12Device* device, uint32_t, uint32_t, uint32_t, uint32_t,
                                             const std::filesystem::path& dll_dir, settings::DlssPreset preset) {
  std::string why;
  const auto bridge = LoadDlssBridge(dll_dir, why);
  if (!bridge) {
    LogLine("[gpu] DLSS unavailable: " + why);
    return nullptr;
  }
  const HMODULE dll = bridge->dll;
  const Gow2DlssApi* api = bridge->api;
  void* ctx;
  if (api->abi >= 2) {
    const Gow2DlssConfig cfg{dll_dir.c_str(), NgxPreset(preset), 0, 0.0f, 0};
    ctx = api->CreateEx(device, &cfg, OnBridgeLog);
  } else {
    if (preset != settings::DlssPreset::kDefault)
      LogLine("[gpu] DLSS: ABI 1 bridge: preset not available");
    ctx = api->Create(device, dll_dir.c_str(), OnBridgeLog);
  }
  if (!ctx) {
    FreeLibrary(dll);
    return nullptr;
  }
  return std::make_unique<DlssUpscaler>(dll, api, ctx);
}

}  // namespace gpu
