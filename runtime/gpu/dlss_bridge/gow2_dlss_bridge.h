// SPDX-License-Identifier: MIT (see LICENSE.txt in this folder)
//
// ABI of gow2_dlss.dll, the DLSS bridge separate from gow2.exe: the
// NVIDIA license forbids the SDK from ending up under a copyleft license, so gow2.exe (GPL-3) loads the DLL
// at runtime and knows only this header, with no NVIDIA types.
//
// Resources: inputs in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output in
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS, states unchanged on exit. Inverted depth, motion vectors
// at render resolution without jitter (from the current pixel to the previous frame, times mv_scale = pixels),
// jitter in render pixels, LDR color.
#pragma once

#include <d3d12.h>
#include <stdint.h>

// ABI 2: Gow2DlssApi grows at the tail (CreateEx and two neural rendering slots). ABI 1 = Create, which is equivalent to
// CreateEx with preset 0. ABI 3: frame generation (FgQuery..FgDestroy at the tail). ABI 4: neural rendering
// intensity and style per frame. ABI 5: neural rendering handed over to OptiScaler; the slots and fields stay, always
// zero, so that the layout does not change.
#define GOW2_DLSS_BRIDGE_ABI 5

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Gow2DlssLogFn)(const char* line);

typedef struct Gow2DlssFrame {
  ID3D12GraphicsCommandList* cmd;
  ID3D12Resource *color, *depth, *motion, *output;
  ID3D12Resource* bias_mask;  // R8_UNORM, 1 = current frame only (HUD); optional (NULL)
  uint32_t render_w, render_h, output_w, output_h;
  float jitter_x, jitter_y, mv_scale_x, mv_scale_y, frame_ms;
  int32_t reset;
} Gow2DlssFrame;

typedef struct Gow2DlssConfig {
  const wchar_t* dll_dir;  // folder of nvngx_dlss.dll (the gow2.exe one)
  uint32_t preset;         // 0 = NVIDIA's choice, otherwise NVSDK_NGX_DLSS_Hint_Render_Preset_* (5 E, 6 F, 10 J, 11 K, 12 L, 13 M)
  int32_t reserved_nr0;    // reserved (neural rendering removed), zero
  float reserved_nr1;
  uint32_t reserved_nr2;
} Gow2DlssConfig;

// Frame generation (NGX feature 11), only abi >= 3.
typedef struct Gow2DlssFgCaps {
  int32_t available, needs_driver;
  uint32_t min_driver_major, min_driver_minor, multi_frame_count_max;
} Gow2DlssFgCaps;

typedef struct Gow2DlssFgConfig {
  const wchar_t* dll_dir;                   // folder of nvngx_dlssg.dll (the gow2.exe one)
  uint32_t width, height, render_w, render_h;  // backbuffer and render resolution (motion and depth)
  DXGI_FORMAT format;                       // backbuffer format
  int32_t hdr;
} Gow2DlssFgConfig;

typedef struct Gow2DlssFgFrame {
  ID3D12GraphicsCommandList* cmd;
  ID3D12Resource *backbuffer, *depth, *motion, *hudless, *output;  // inputs NON_PIXEL_SHADER_RESOURCE, output UAV
  uint32_t render_w, render_h;
  // Jitter in render pixels; mv_scale brings the motion vectors to render pixels (like SR, see the top; the
  // FG guide wants pixels and says 1 if they already are, even though the mvecScale comment in the NGX header says
  // "normalize").
  float jitter_x, jitter_y, mv_scale_x, mv_scale_y;
  // Row-major, v' = v * M (dlss_fg_math.h), no jitter.
  float clip_to_prev_clip[16], prev_clip_to_clip[16], view_to_clip[16], clip_to_view[16];
  float near_z, far_z, fov_y, aspect;  // fov_y vertical in radians; aspect = width / height
  int32_t reset, depth_inverted;
} Gow2DlssFgFrame;

typedef struct Gow2DlssApi {
  uint32_t abi;      // GOW2_DLSS_BRIDGE_ABI
  const char* name;  // e.g. "DLSS 310.9.16"
  // NULL if DLSS is not available (reason passed to log). dll_dir: the only folder of nvngx_dlss.dll.
  void* (*Create)(ID3D12Device* device, const wchar_t* dll_dir, Gow2DlssLogFn log);
  // Records DLSS into frame->cmd (creates or recreates the feature if the dimensions change). 0 = ok.
  int32_t (*Evaluate)(void* ctx, const Gow2DlssFrame* frame);
  // The GPU must no longer use ctx.
  void (*Destroy)(void* ctx);
  // Only abi >= 2.
  void* (*CreateEx)(ID3D12Device* device, const Gow2DlssConfig* config, Gow2DlssLogFn log);
  void* reserved_nr[2];  // were EvaluateNr and NrAvailable; always NULL
  // Only abi >= 3. NGX opened if needed (shared with SR). 0 = caps filled.
  int32_t (*FgQuery)(ID3D12Device* device, const wchar_t* dll_dir, Gow2DlssFgCaps* out, Gow2DlssLogFn log);
  // Creates feature 11 recording into cmd (the caller executes and waits). NULL = not created (reason in the log).
  void* (*FgCreate)(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const Gow2DlssFgConfig* config,
                    Gow2DlssLogFn log);
  int32_t (*FgEvaluate)(void* ctx, const Gow2DlssFgFrame* frame);  // 0 = output scritto
  const char* (*FgDescription)(void* ctx);  // e.g. "DLSS FG 310.9.16, model 3" ("" if NGX says nothing)
  void (*FgDestroy)(void* ctx);              // the GPU must no longer use ctx
} Gow2DlssApi;

typedef const Gow2DlssApi* (*Gow2DlssGetApiFn)(void);
#define GOW2_DLSS_GET_API "Gow2DlssGetApi"

#ifdef __cplusplus
}
#endif
