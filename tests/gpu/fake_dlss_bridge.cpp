// Fake DLSS bridge for upscaler_dlss_test (same ABI as runtime/gpu/dlss_bridge/gow2_dlss_bridge.h,
// no SDK). Behavior chosen by the environment variables read on every call:
// GOW2_FAKE_DLSS_ABI (reported abi), GOW2_FAKE_DLSS_CREATE_FAIL (Create returns NULL),
// GOW2_FAKE_DLSS_EVAL_RESULT (Evaluate's return). FakeDlssLastFrame: the last frame received.
// ABI 2: CreateEx records the Gow2DlssConfig (FakeDlssLastPreset = preset received, -1 if CreateEx was not
// called; FakeDlssLastConfig).
// ABI 3: GOW2_FAKE_DLSS_FG=0 -> FgQuery reports available = 0 (GOW2_FAKE_DLSS_FG_NEEDS_DRIVER=1: needs_driver,
// minimum driver 555.85); GOW2_FAKE_DLSS_FG_CREATE_FAIL=1 -> FgCreate NULL; GOW2_FAKE_DLSS_FG_DESC=0 -> FgDescription
// "", GOW2_FAKE_DLSS_FG_EVAL_FAIL=1 -> FgEvaluate returns -2. FakeDlssFgEvaluations: calls to FgEvaluate; FakeDlssLastFgFrame / FakeDlssLastFgConfig: the last ones received.
#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "gpu/dlss_bridge/gow2_dlss_bridge.h"

namespace {

Gow2DlssFrame last{};
Gow2DlssConfig last_config{};
bool config_seen = false;
int ctx_token = 0;

int Env(const char* name, int fallback) {
  char buf[32];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n && n < sizeof(buf) ? std::atoi(buf) : fallback;
}

void* Create(ID3D12Device*, const wchar_t*, Gow2DlssLogFn log) {
  if (Env("GOW2_FAKE_DLSS_CREATE_FAIL", 0)) {
    log("creation rejected by the fake");
    return nullptr;
  }
  log("fake ready");
  return &ctx_token;
}

int32_t Evaluate(void* ctx, const Gow2DlssFrame* f) {
  if (ctx != &ctx_token) return -1;
  last = *f;
  return Env("GOW2_FAKE_DLSS_EVAL_RESULT", 0);
}

void* CreateEx(ID3D12Device* device, const Gow2DlssConfig* cfg, Gow2DlssLogFn log) {
  last_config = *cfg;
  config_seen = true;
  return Create(device, cfg->dll_dir, log);
}

void Destroy(void*) {}

uint32_t fg_evaluations = 0;
Gow2DlssFgFrame last_fg{};
Gow2DlssFgConfig last_fg_config{};

int32_t FgQuery(ID3D12Device*, const wchar_t*, Gow2DlssFgCaps* out, Gow2DlssLogFn) {
  *out = {Env("GOW2_FAKE_DLSS_FG", 1) ? 1 : 0, Env("GOW2_FAKE_DLSS_FG_NEEDS_DRIVER", 0), 555, 85, 1};
  return 0;
}

void* FgCreate(ID3D12Device*, ID3D12GraphicsCommandList*, const Gow2DlssFgConfig* cfg, Gow2DlssLogFn log) {
  if (Env("GOW2_FAKE_DLSS_FG_CREATE_FAIL", 0)) {
    log("FG feature rejected by the fake");
    return nullptr;
  }
  last_fg_config = *cfg;
  return new int(0);
}

int32_t FgEvaluate(void* ctx, const Gow2DlssFgFrame* f) {
  if (!ctx) return -1;
  ++fg_evaluations;
  last_fg = *f;
  return Env("GOW2_FAKE_DLSS_FG_EVAL_FAIL", 0) ? -2 : 0;
}

const char* FgDescription(void*) { return Env("GOW2_FAKE_DLSS_FG_DESC", 1) ? "fake DLSS FG" : ""; }

void FgDestroy(void* ctx) { delete static_cast<int*>(ctx); }

Gow2DlssApi api{};

}  // namespace

extern "C" __declspec(dllexport) const Gow2DlssApi* Gow2DlssGetApi(void) {
  api = {uint32_t(Env("GOW2_FAKE_DLSS_ABI", GOW2_DLSS_BRIDGE_ABI)), "fake DLSS", Create, Evaluate, Destroy, CreateEx, {nullptr, nullptr},
         FgQuery, FgCreate, FgEvaluate, FgDescription, FgDestroy};
  return &api;
}

extern "C" __declspec(dllexport) const Gow2DlssFrame* FakeDlssLastFrame(void) { return &last; }
extern "C" __declspec(dllexport) const Gow2DlssConfig* FakeDlssLastConfig(void) { return &last_config; }
extern "C" __declspec(dllexport) uint32_t FakeDlssFgEvaluations(void) { return fg_evaluations; }
extern "C" __declspec(dllexport) const Gow2DlssFgFrame* FakeDlssLastFgFrame(void) { return &last_fg; }
extern "C" __declspec(dllexport) const Gow2DlssFgConfig* FakeDlssLastFgConfig(void) { return &last_fg_config; }

// Preset received by CreateEx, -1 if it was never called (ABI 1 bridge or Create).
extern "C" __declspec(dllexport) int FakeDlssLastPreset(void) { return config_seen ? int(last_config.preset) : -1; }
