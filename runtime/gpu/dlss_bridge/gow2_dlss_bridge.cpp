// SPDX-License-Identifier: MIT (see LICENSE.txt in this folder)
//
// gow2_dlss.dll: DLSS Super Resolution and Frame Generation with NGX (NVIDIA SDK, separate license, not in the
// repository) behind the ABI of gow2_dlss_bridge.h. One feature per context, created on the first frame's
// command list and recreated when the dimensions change. Frame generation (feature 11) has its own contexts, in the same
// opened NGX.
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>

#include <initializer_list>

#include "gow2_dlss_bridge.h"
#include "gow2_dlss_policy.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

#define GOW2_DLSS_SDK_VERSION "310.9.16"

namespace {

// Fixed ProjectId of the project (NVSDK_NGX_ENGINE_TYPE_CUSTOM: no ID assigned by NVIDIA).
constexpr char kProjectId[] = "5b2c0b8e-3f0f-4c2a-9a3e-6f1d2c7b9e41";

struct Context {
  ID3D12Device* device;
  NVSDK_NGX_Parameter* params;
  NVSDK_NGX_Handle* feature;
  uint32_t render_w, render_h, output_w, output_h;
  Gow2DlssLogFn log;
  uint32_t preset;
};

void Logf(Gow2DlssLogFn log, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  log(buf);
}

NVSDK_NGX_PerfQuality_Value NgxQuality(uint32_t render_w, uint32_t output_w) {
  switch (gow2_dlss::QualityFor(render_w, output_w)) {
    case gow2_dlss::Quality::kDlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case gow2_dlss::Quality::kMaxQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case gow2_dlss::Quality::kBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case gow2_dlss::Quality::kMaxPerf: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case gow2_dlss::Quality::kUltraPerf: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
  }
  return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

const char* QualityName(uint32_t render_w, uint32_t output_w) {
  switch (gow2_dlss::QualityFor(render_w, output_w)) {
    case gow2_dlss::Quality::kDlaa: return "DLAA";
    case gow2_dlss::Quality::kMaxQuality: return "Quality";
    case gow2_dlss::Quality::kBalanced: return "Balanced";
    case gow2_dlss::Quality::kMaxPerf: return "Performance";
    case gow2_dlss::Quality::kUltraPerf: return "Ultra Performance";
  }
  return "?";
}

const char* PresetName(uint32_t preset) {
  switch (preset) {
    case 0: return "default";
    case 5: return "E";
    case 6: return "F";
    case 10: return "J";
    case 11: return "K";
    case 12: return "L";
    case 13: return "M";
  }
  return "other";
}

// Live contexts and the device on which NGX is open. CreateEx and Destroy come from several threads (main, command
// processor) but never together: the runtime calls them in sequence.
int g_contexts = 0;
ID3D12Device* g_ngx_device = nullptr;

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature feature) {
  fprintf(stderr, "[NGX %d] %s", int(feature), message);
}

// Opens NGX on device if no context (SR or FG) holds it open. Whoever then does not create a context closes with
// CloseNgxIfUnused.
bool OpenNgx(ID3D12Device* device, const wchar_t* dll_dir, Gow2DlssLogFn log) {
  const wchar_t* paths[] = {dll_dir};
  NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  // GOW2_DLSS_NGX_LOG=1: detailed NGX log (also from the driver threads) on stderr, for diagnosis.
  if (const char* v = getenv("GOW2_DLSS_NGX_LOG"); v && *v == '1') {
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
  }
  // NGX stays open as long as there is a context (g_contexts). The runtime keeps the probe made at startup alive
  // (d3d12_backend.cpp) for the whole session, so NGX does not close and reopen when the presenter destroys and
  // recreates the context: NGX proxies like OptiScaler do not survive closing and reopening.
  if (g_contexts > 0 && device != g_ngx_device) {
    log("NGX already open on another device");
    return false;
  }
  if (g_contexts == 0) {
    const NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dll_dir, device, &info);
    if (NVSDK_NGX_FAILED(r)) {
      Logf(log, "NGX initialization failed (0x%08x): NVIDIA driver missing or GPU not supported", unsigned(r));
      return false;
    }
    g_ngx_device = device;
  }
  return true;
}

void CloseNgxIfUnused(ID3D12Device* device) {
  if (g_contexts == 0) NVSDK_NGX_D3D12_Shutdown1(device);
}

void* CreateEx(ID3D12Device* device, const Gow2DlssConfig* cfg, Gow2DlssLogFn log) {
  if (!device || !cfg) {
    log("no D3D12 device");
    return nullptr;
  }
  if (!OpenNgx(device, cfg->dll_dir, log)) return nullptr;
  NVSDK_NGX_Parameter* params = nullptr;
  const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
  int available = 0, needs_driver = 0;
  if (NVSDK_NGX_SUCCEED(r)) {
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  }
  if (NVSDK_NGX_FAILED(r) || !available || needs_driver) {
    if (needs_driver) {
      unsigned major = 0, minor = 0;
      NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
      NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
      Logf(log, "a newer NVIDIA driver is required (at least %u.%u)", major, minor);
    } else {
      Logf(log, "DLSS not supported by this GPU or nvngx_dlss.dll missing (0x%08x)", unsigned(r));
    }
    if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
    CloseNgxIfUnused(device);
    return nullptr;
  }
  ++g_contexts;
  return new Context{device, params, nullptr, 0, 0, 0, 0, log, cfg->preset};
}

void* Create(ID3D12Device* device, const wchar_t* dll_dir, Gow2DlssLogFn log) {
  const Gow2DlssConfig cfg{dll_dir, 0, 0, 0.0f, 0};
  return CreateEx(device, &cfg, log);
}

bool EnsureFeature(Context& c, const Gow2DlssFrame& f) {
  if (c.feature && c.render_w == f.render_w && c.render_h == f.render_h && c.output_w == f.output_w &&
      c.output_h == f.output_h)
    return true;
  // The caller guarantees the GPU no longer uses the previous feature (dimensions changed with the GPU idle).
  if (c.feature) NVSDK_NGX_D3D12_ReleaseFeature(c.feature);
  c.feature = nullptr;
  NVSDK_NGX_DLSS_Create_Params p{};
  p.Feature.InWidth = f.render_w;
  p.Feature.InHeight = f.render_h;
  p.Feature.InTargetWidth = f.output_w;
  p.Feature.InTargetHeight = f.output_h;
  p.Feature.InPerfQualityValue = NgxQuality(f.render_w, f.output_w);
  p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  if (c.preset != 0)
    for (const char* name :
         {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
      NVSDK_NGX_Parameter_SetUI(c.params, name, c.preset);
  const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(f.cmd, 1, 1, &c.feature, c.params, &p);
  if (NVSDK_NGX_FAILED(r)) {
    Logf(c.log, "feature creation %ux%u -> %ux%u failed (0x%08x)", f.render_w, f.render_h, f.output_w,
         f.output_h, unsigned(r));
    c.feature = nullptr;
    return false;
  }
  c.render_w = f.render_w, c.render_h = f.render_h, c.output_w = f.output_w, c.output_h = f.output_h;
  Logf(c.log, "feature %ux%u -> %ux%u, mode %s, preset %s", f.render_w, f.render_h, f.output_w, f.output_h,
       QualityName(f.render_w, f.output_w), PresetName(c.preset));
  return true;
}

int32_t Evaluate(void* ctx, const Gow2DlssFrame* f) {
  if (!ctx || !f || !f->cmd) return -1;
  Context& c = *static_cast<Context*>(ctx);
  if (!EnsureFeature(c, *f)) return -2;
  NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
  e.Feature.pInColor = f->color;
  e.Feature.pInOutput = f->output;
  e.pInDepth = f->depth;
  e.pInMotionVectors = f->motion;
  e.pInBiasCurrentColorMask = f->bias_mask;
  e.InJitterOffsetX = f->jitter_x;
  e.InJitterOffsetY = f->jitter_y;
  e.InRenderSubrectDimensions = {f->render_w, f->render_h};
  e.InReset = f->reset;
  e.InMVScaleX = f->mv_scale_x;
  e.InMVScaleY = f->mv_scale_y;
  e.InPreExposure = 1.0f;
  e.InExposureScale = 1.0f;
  e.InFrameTimeDeltaInMsec = f->frame_ms;
  const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(f->cmd, c.feature, c.params, &e);
  if (NVSDK_NGX_FAILED(r)) {
    Logf(c.log, "evaluate failed (0x%08x)", unsigned(r));
    return -3;
  }
  return 0;
}

void Destroy(void* ctx) {
  if (!ctx) return;
  Context* c = static_cast<Context*>(ctx);
  if (c->feature) NVSDK_NGX_D3D12_ReleaseFeature(c->feature);
  NVSDK_NGX_D3D12_DestroyParameters(c->params);
  --g_contexts;
  CloseNgxIfUnused(c->device);
  delete c;
}

// Frame generation: feature 11 (nvngx_dlssg.dll, loaded and verified by NGX), one generated frame per rendered
// frame (multiFrameCount = 1). Counts in g_contexts like SR.
struct FgContext {
  ID3D12Device* device;
  NVSDK_NGX_Parameter* params;
  NVSDK_NGX_Handle* feature;
  Gow2DlssLogFn log;
  int32_t hdr;
  char description[64];
};

int32_t FgQuery(ID3D12Device* device, const wchar_t* dll_dir, Gow2DlssFgCaps* out, Gow2DlssLogFn log) {
  if (!device || !out) return -1;
  *out = {};
  if (!OpenNgx(device, dll_dir, log)) return -2;
  NVSDK_NGX_Parameter* caps = nullptr;
  const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  if (NVSDK_NGX_SUCCEED(r)) {
    unsigned available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &out->min_driver_major);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &out->min_driver_minor);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &out->multi_frame_count_max);
    out->available = available ? 1 : 0;
    out->needs_driver = needs_driver ? 1 : 0;
  } else {
    Logf(log, "frame generation: NGX capabilities not read (0x%08x)", unsigned(r));
  }
  if (caps) NVSDK_NGX_D3D12_DestroyParameters(caps);
  CloseNgxIfUnused(device);
  return NVSDK_NGX_SUCCEED(r) ? 0 : -3;
}

void* FgCreate(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const Gow2DlssFgConfig* cfg,
               Gow2DlssLogFn log) {
  if (!device || !cmd || !cfg) {
    log("frame generation: missing device, command list or configuration");
    return nullptr;
  }
  if (!OpenNgx(device, cfg->dll_dir, log)) return nullptr;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&params);
  NVSDK_NGX_Handle* feature = nullptr;
  if (NVSDK_NGX_SUCCEED(r)) {
    NVSDK_NGX_DLSSG_Create_Params p{};
    p.Width = cfg->width;
    p.Height = cfg->height;
    p.NativeBackbufferFormat = cfg->format;
    p.RenderWidth = cfg->render_w;
    p.RenderHeight = cfg->render_h;
    r = NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &feature, params, &p);
  }
  if (NVSDK_NGX_FAILED(r) || !feature) {
    Logf(log, "frame generation: feature %ux%u (render %ux%u) not created (0x%08x)", cfg->width, cfg->height,
         cfg->render_w, cfg->render_h, unsigned(r));
    if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
    CloseNgxIfUnused(device);
    return nullptr;
  }
  ++g_contexts;
  Logf(log, "frame generation: feature %ux%u (render %ux%u)", cfg->width, cfg->height, cfg->render_w, cfg->render_h);
  return new FgContext{device, params, feature, log, cfg->hdr, {}};
}

int32_t FgEvaluate(void* ctx, const Gow2DlssFgFrame* f) {
  if (!ctx || !f || !f->cmd) return -1;
  FgContext& c = *static_cast<FgContext*>(ctx);
  NVSDK_NGX_D3D12_DLSSG_Eval_Params e{};
  e.pBackbuffer = f->backbuffer;
  e.pDepth = f->depth;
  e.pMVecs = f->motion;
  e.pHudless = f->hudless;
  e.pOutputInterpFrame = f->output;
  NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
  o.multiFrameCount = 1;
  o.multiFrameIndex = 1;
  memcpy(o.clipToPrevClip, f->clip_to_prev_clip, sizeof(o.clipToPrevClip));
  memcpy(o.prevClipToClip, f->prev_clip_to_clip, sizeof(o.prevClipToClip));
  memcpy(o.cameraViewToClip, f->view_to_clip, sizeof(o.cameraViewToClip));
  memcpy(o.clipToCameraView, f->clip_to_view, sizeof(o.clipToCameraView));
  for (int i = 0; i < 4; ++i) o.clipToLensClip[i][i] = 1.0f;  // no lens distortion
  o.jitterOffset[0] = f->jitter_x;
  o.jitterOffset[1] = f->jitter_y;
  o.mvecScale[0] = f->mv_scale_x;
  o.mvecScale[1] = f->mv_scale_y;
  // shortcut: the camera position and axes in the world do not come from the game: origin and canonical axes. To
  // be added to the ABI if FG quality suffers (the guide lists them among the camera data).
  o.cameraUp[1] = o.cameraRight[0] = o.cameraFwd[2] = 1.0f;
  o.cameraNear = f->near_z;
  o.cameraFar = f->far_z;
  o.cameraFOV = f->fov_y;
  o.cameraAspectRatio = f->aspect;
  o.colorBuffersHDR = c.hdr != 0;
  o.depthInverted = f->depth_inverted != 0;
  o.cameraMotionIncluded = true;
  o.reset = f->reset != 0;
  // Motion and depth at render resolution (they may live in larger resources).
  o.mvecsSubrectSize = {f->render_w, f->render_h};
  o.depthSubrectSize = {f->render_w, f->render_h};
  const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(f->cmd, c.feature, c.params, &e, &o);
  if (NVSDK_NGX_FAILED(r)) {
    Logf(c.log, "frame generation: evaluate failed (0x%08x)", unsigned(r));
    return -3;
  }
  return 0;
}

// The model is read after executing the FgCreate command list (as in the spike's probe).
const char* FgDescription(void* ctx) {
  if (!ctx) return "";
  FgContext& c = *static_cast<FgContext*>(ctx);
  unsigned model = 0;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(c.params, "DLSSG.ModelVersion", &model)))
    snprintf(c.description, sizeof(c.description), "DLSS FG " GOW2_DLSS_SDK_VERSION ", model %u", model);
  else
    c.description[0] = 0;
  return c.description;
}

void FgDestroy(void* ctx) {
  if (!ctx) return;
  FgContext* c = static_cast<FgContext*>(ctx);
  NVSDK_NGX_D3D12_ReleaseFeature(c->feature);
  NVSDK_NGX_D3D12_DestroyParameters(c->params);
  --g_contexts;
  CloseNgxIfUnused(c->device);
  delete c;
}

const Gow2DlssApi kApi = {GOW2_DLSS_BRIDGE_ABI, "DLSS " GOW2_DLSS_SDK_VERSION,
                          Create,               Evaluate,
                          Destroy,              CreateEx,
                          {nullptr, nullptr},
                          FgQuery,              FgCreate,
                          FgEvaluate,           FgDescription,
                          FgDestroy};

}  // namespace

extern "C" __declspec(dllexport) const Gow2DlssApi* Gow2DlssGetApi(void) { return &kApi; }
