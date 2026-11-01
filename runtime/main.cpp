// gow2.exe [--data <dir>] [--user-data <dir>] [--trace] [--run-seconds N] [--language <code>]
//          [--gpu fake|d3d12] [--screenshot S:file] [--gpu-cvar name=value]... [--gpu-probe]
//          [--shader-capture]
//          [--audio xaudio2|null] [--mute] [--audio-capture <file.wav>] [--xma-decoder new|old]
//          [--input-script "<s>:<BUTTON>[+<ms>],..."]
//          [--config <file>] [--window windowed|borderless|fullscreen] [--resolution WxH]
//          [--vsync on|off] [--frame-rate 30|60|120|144|monitor|unlimited] [--vrr auto|on|off]
//          [--frame-generation off|fsr|dlss] [--aspect 16:9|21:9|16:10]
//          [--hdr on|off|force] [--hdr-peak-nits <n>] [--hdr-paper-white-nits <n>]
//          [--shadow-quality low|original|high] [--dynamic-shadows on|off]
//          [--aa off|fxaa|fxaa_extreme] [--sharpening off|0.0..1.0]
//          [--internal-resolution 1|2|3] [--anisotropic 0|2|4|8|16] [--enhanced-shaders on|off]
//          [--upscaler off|fsr|dlss] [--dlss-preset default|e|f|j|k|l|m] [--dlss-mode auto|dlaa|...] [--skip-movie <name>]... [--launcher]
//
// With no arguments (double click) or with --launcher (without --help) the launcher
// (runtime/launcher) opens first: settings of gow2.toml (or --config); Play saves them and
// starts the game, Quit closes with 0. If the launcher cannot start (no D3D11) the game
// runs with the file as it is.
//
// Loads <dir>/default.xex (default: the profile's data folder, `game_data` for GoW2 and
// `game_data_gow3` for GoW3, in the current directory, otherwise next to the executable) into guest memory and enters the recompiled entry
// point. The kernel and XAM imports implemented in runtime/kernel/ (KERNEL_EXPORT) run
// in HLE; the others are stubs that write `[kernel] UNIMPLEMENTED Name(r3..r10)` to the
// log (gow2.log in the current directory) on the first call and return 0. With --trace
// the stubs log every call. On a normal exit (entry returned) the log receives the
// summary, `[summary] ...` lines. --trace also logs the calls to the implemented imports
// (KERNEL_EXPORT). --run-seconds N: after N seconds a host thread writes the summary and
// exits with 0. The summary is written only once (kernel::stats::WriteSummary, which
// first stops the XMA decoder and the audio with apu::StopXmaDecoder and apu::Shutdown
// and closes the WAV capture) by every orderly exit: timeout, entry returned, window
// closed, HalReturnToFirmware (exit 0), KeBugCheck (exit 4), GPU errors (exit 5: device
// not created, xe::FatalError from the Xenia code, device lost).
// The crash handler (exit 3) does NOT write it: Summary() takes locks and allocates, and
// the crashing thread may hold the counters' or the heap's lock.
//
// Guest file system (kernel/io/vfs.h): game:, d: and \Device\Cdrom0 are mounted read-only
// on <data>; cache: read/write on <user_data>/cache (created if missing). <user_data> is
// --user-data if given, otherwise <parent of data>/<profile user_data folder> (`user_data` for
// GoW2, `user_data_gow3` for GoW3): with the default data folder it is in the repository root (git-ignored); with --data
// elsewhere the user data sits next to the game data. XAM content (saves...) lives in
// <user_data>/content: every package opened with XamContentCreateEx is a mounted
// subfolder (read and write) with the root name chosen by the game (kernel/xam.h).
// <user_data>/shaders: Xenia's persistent shader/pipeline storage of the d3d12 backend, files named
// by the XEX title id (gpu/d3d12_backend.h, D3D12Options::shader_cache_root).
//
// --language <code>: language returned by XGetLanguage (default en, or [game] language of
// the file): en ja de fr es it ko zh (= zh-tw, Traditional Chinese) zh-cn pt pl ru,
// case-insensitive. An unknown code is a usage error: message on stderr and exit 2 (it
// does not carry on silently with a different language).
//
// --gpu fake|d3d12: GPU backend (gpu/gpu_system.h; default d3d12). d3d12 = 1280x720 game
// window (platform/), D3D12 device, Xenia's D3D12 command processor on the game's ring
// and DXGI presenter (gpu/d3d12_backend.h). fake = fake GPU with no renderer and no
// window (gpu_fake/), used by the tests. Any other value (even in uppercase) is a usage
// error: exit 2. If the D3D12 device cannot be created: exit 5 with a message suggesting
// --gpu fake.
// --screenshot S:file (repeatable): the first frame presented S seconds (0..86400) after
// startup goes to `file` (24-bit BMP); multiple requests are served in time order, one
// per frame; d3d12 only.
// --shader-capture (debug, not in the launcher or the F1 overlay): F11 starts/stops a recording in
// <exe folder>/shader_captures/<game id>/<date_time>/ of the shaders used (shaders/) and of 5 PNG shots per
// second, each with the list of its frame's draws (frames/); d3d12 only (gpu/shader_capture.h).
// --gpu-cvar name=value (repeatable): cvar of Xenia's GPU code (gow2::xenia::SetCvar),
// applied in order after gow2's defaults render_target_path_d3d12=rtv,
// draw_resolution_scaled_texture_offsets=false (tfetch offsets in guest texels for UE3's
// DOF downsample at internal resolution > 1), occlusion_query=strict (true occlusion
// query counts), d3d12_submit_every_draws=100, d3d12_submit_every_draws_scaled (true only
// for the games whose profile asks for it), gow2_clear_msaa_remap=true,
// gow2_resolve_fill_depth=false and draw_resolution_scale_threshold=640.
// Unknown name or invalid value: exit 2.
// --config <file>: TOML settings file (runtime/settings.h; default gow2.toml next to
// gow2.exe, missing = default values). Sections [video]
// (mode, width, height, scaling, vsync, resizable, antialiasing, sharpening, sharpness,
// internal_resolution, anisotropic, enhanced_shaders, upscaler, vrr, frame_generation, hdr, hdr_peak_nits, hdr_paper_white_nits, aspect, dlss_preset, dlss_mode), [game] (language, frame_rate), [engine] (shadow_quality, dynamic_shadows; world_textures accepted and ignored), [patches] (show_fps, disable_motion_blur, disable_lens_flares, disable_bloom: true/false, default false, see README) and [gpu] (cvars = { name = "value" },
// applied like --gpu-cvar before the command-line ones). Invalid file:
// message "file:line: key: reason" on stderr and exit 2.
// --window windowed|borderless|fullscreen, --resolution WxH (320..7680 x 240..4320),
// --vsync on|off, --frame-rate 30|60|120|144|monitor|unlimited, --vrr auto|on|off (fps limit below the
// refresh rate, see README; used by the frame pacing), --frame-generation off|fsr|dlss (FSR 3 or DLSS; "on" = fsr, see README), --aspect 16:9|21:9|16:10 (scene aspect ratio, see README), --dlss-preset default|e|f|j|k|l|m (DLSS preset), --dlss-mode auto|dlaa|quality|balanced|performance|ultra_performance (DLSS mode: if not auto it decides the internal resolution), --hdr on|off|force (HDR10 output; force even on an SDR output, for testing), --hdr-peak-nits <n> (0 or 100-10000), --hdr-paper-white-nits <n> (0 or 80-1000), --shadow-quality low|original|high, --dynamic-shadows on|off
// (engine configuration, see README), --aa off|fxaa|fxaa_extreme, --sharpening off|0.0..1.0 (CAS, strength),
// --internal-resolution 1|2|3 (render resolution multiplier),
// --anisotropic 0|2|4|8|16 (anisotropic filtering forced via the anisotropic_override cvar, default 16, 0 = the game's own),
// --enhanced-shaders on|off (replacement shaders by hash, default on), --upscaler off|fsr|dlss
// (temporal upscaler, gpu/upscaler.h): they override the file (default < gow2.toml < command line).
// --skip-movie <name> (repeatable): in place of <name>.bik the game opens LoadingMovie.bik
// padded with zeros up to the original's size, so that movie lasts an instant (for the
// tests: EpicLogo, MGSLogo, Attract_Opening_Cinematic).
// The presenter's back buffer always follows the client area (the monitor in
// borderless): width/height set the client area in windowed and, in fullscreen
// (DXGI exclusive, re-entered when focus returns), the closest video mode (default: the
// monitor's current mode). The guest output is drawn into it with fit or stretch scaling.
// --gpu-probe: prints the first hardware D3D12 adapter on stdout and exits with 0, or
// with 5 if there is none (the game does not start: no log and no summary).
// --audio xaudio2|null: audio output (apu/audio_system.h; default xaudio2, with
// automatic fallback to the null sink, paced at 187.5 buffers/s). Any other value (even
// in uppercase) is a usage error: exit 2. --mute: no sound, same pace (the capture does
// not change). --audio-capture <file.wav>: the frames sent by the game go to a 48 kHz
// 5.1 float WAV; file that cannot be opened: exit 2. --xma-decoder new|old: xenia-canary
// XMA context (apu/xma_decoder.h; default new, old as a fallback); other value: exit 2.
// Input (hid/drivers.h): drivers in this order: script (only with --input-script),
// XInput 1.4 pads (users 0..3, if xinput1_4.dll is present), SDL pads, window keyboard
// (always last: virtual pad 0 is always connected; with --gpu fake there is no window
// and its state is null). Keyboard, only with the window focused: Enter = START,
// Esc = BACK, Space = A, WASD = left stick, arrows = d-pad.
// --input-script "<s>:<BUTTON>[+<ms>],...": presses of virtual pad 0 s seconds after
// startup (at most 3 decimals) for ms milliseconds (1..60000, default 100); BUTTON one
// of DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT START BACK LTHUMB RTHUMB LB RB A B X Y, or
// LS_UP LS_DOWN LS_LEFT LS_RIGHT (left stick fully pushed) LT RT (trigger fully pressed).
// Invalid: exit 2.
//
// Exit: 0 if the entry point returns, when --run-seconds elapses or when the window is
// closed; 1 for a runtime error; 2 for incorrect usage (also unknown --audio,
// --audio-capture that cannot be opened or invalid --input-script) or if default.xex is
// missing; 3 for a crash (unhandled exception); 4 for a game KeBugCheck (the
// `[kernel] KeBugCheck code=... caller=...` line is the last in the log before the
// summary); 5 for a GPU error (D3D12 device not created, `[gpu] device removed reason=...`,
// xe::FatalError from the Xenia code; --gpu-probe without an adapter).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// First: the generated ppc_config.h (kernel/function.h, included by
// kernel/threads.h, would skip it).
#include "recomp_glue.h"

#include "apu/audio_system.h"
#include "apu/xma_decoder.h"
#include "hid/drivers.h"
#include "hid/input_script.h"
#include "hid/input_system.h"
#include "hid/sdl_driver.h"
#include "platform/sdl_gamepads.h"
#include "crash_map.h"
#include "frame_pacing.h"
#include "game_data.h"
#include "game_profile.h"
#include "gpu/d3d12_backend.h"
#include "gpu/d3d12_probe.h"
#include "gpu/gpu_system.h"
#include "gpu/screenshot.h"
#include "gpu_fake/gpu.h"
#include "guest_memory.h"
#include "image_loader.h"
#include "imports.h"
#include "kernel/function.h"
#include "kernel/guest.h"
#include "kernel/io/vfs.h"
#include "kernel/memory.h"
#include "kernel/module.h"
#include "kernel/rtl_exports.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "kernel/xam.h"
#include "kernel/xma.h"
#include "launcher/launcher.h"
#include "launcher/optiscaler.h"
#include "launcher/launcher_model.h"
#include "log.h"
#include "physical_watch.h"
#include "platform/platform.h"
#include "settings.h"
#include "game_patches/game_patches.h"
#include "engine_config.h"

// kernel/threads.h pulls in xbox.h and therefore windows.h, which defines LoadImage
// as a macro (LoadImageA/W): the one from image_loader.h is needed.
#undef LoadImage

namespace fs = std::filesystem;

namespace {

// Guest stack of the main thread (the others use the requested one or the XEX
// header). Below it sits the kernel::kStackGuardSize guard: an overflow fails in the
// function that writes (crash "stack overflow").
constexpr uint32_t kStackSize = 1024 * 1024;

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

std::string Show(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

// Command-line argument in UTF-8.
std::string Narrow(std::wstring_view arg) {
  try {
    return Show(fs::path(arg));
  } catch (...) {
    std::string out;
    for (wchar_t c : arg) out.push_back(c < 0x80 ? char(c) : '?');
    return out;
  }
}

// stderr line for a startup error; a message box too when no console/pipe is attached (double click).
void ErrPuts(const std::string& text) {
  std::fputs(text.c_str(), stderr);
  if (!plat::OutputVisible()) plat::ShowErrorBox(text);
}

int Fail(int code, const std::string& message) {
  LogLine("gow2: " + message);
  if (!plat::OutputVisible()) plat::ShowErrorBox(message);
  return code;
}

// Before the summary (kernel::stats::SetBeforeSummaryHook): XMA decoder stopped
// (xma_* counters final), then the Audio Worker and the WAV capture.
void StopAudio() {
  apu::StopXmaDecoder();
  apu::Shutdown();
}

void OnCrash(const plat::CrashInfo& info) {
  char line[256];  // no heap and no locks in the exception filter
  FormatCrashLine(line, sizeof(line), info);  // guards of all guest threads
  LogLineFromCrash(line);
  // Guest registers (those written to memory by the generated code; the values
  // held in host registers may be stale).
  if (const PPCContext* ctx = kernel::GetPPCContext()) {
    std::snprintf(line, sizeof(line),
                  "crash guest ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X",
                  ctx->ctr.u32, ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32,
                  ctx->r9.u32, ctx->r10.u32, ctx->r11.u32);
    LogLineFromCrash(line);
  }
  // Heuristic backtrace: values on the host stack that fall in the recompiled code
  // (return addresses, but also leftovers: read it with judgment).
  if (info.stack_pointer && info.stack_base > info.stack_pointer) {
    int n = std::snprintf(line, sizeof(line), "crash stack");
    uint32_t last = 0;
    int found = 0;
    for (uintptr_t p = info.stack_pointer; p + 8 <= info.stack_base && found < 16; p += 8) {
      const uint32_t guest = GuestFunctionForHostPc(*reinterpret_cast<const uintptr_t*>(p));
      if (!guest || guest == last) continue;
      last = guest;
      ++found;
      if (n > 0 && size_t(n) < sizeof(line)) n += std::snprintf(line + n, sizeof(line) - size_t(n), " %08X", guest);
    }
    LogLineFromCrash(line);
  }
  // No StopVibrationBeforeExit here: the crashing thread may hold the InputSystem
  // lock or the SDL rumble mutex, and the stop allocates and waits. After a crash the
  // pads may keep vibrating.
  std::_Exit(3);
}

// Pad vibrations do not expire on their own (XInput and SDL with duration 0) and the
// exits go through std::_Exit, without destructors: before exiting they are all
// zeroed, without ever waiting more than a few tens of ms.
void StopVibrationBeforeExit() {
  hid::StopAllVibration(std::chrono::milliseconds(20));
  plat::SdlGamepads::StopAllRumble(std::chrono::milliseconds(30));
}

// OptiScaler installation from the overlay: _Exit must not cut it off mid-copy (swapped dxgi.dll,
// half-copied OptiScaler folder). The download is cancelled; a copy in progress finishes (a few local files).
// Also the shader capture's queued shots (--shader-capture), written before exiting.
void StopInstallBeforeExit() {
  optiscaler::CancelInstall();
  optiscaler::WaitInstallCopy();
  gpu::StopShaderCaptureBeforeExit();
}

// --run-seconds: host thread that after `seconds` writes the summary and ends the
// process with exit 0, even with guest threads blocked or running (no static
// destructors under their feet: _Exit, like the normal exit).
void StartWatchdog(uint32_t seconds) {
  std::thread([seconds] {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    LogLine("gow2: --run-seconds " + std::to_string(seconds) + " elapsed");
    StopVibrationBeforeExit();
    kernel::stats::WriteSummary();
    StopInstallBeforeExit();
    std::_Exit(0);
  }).detach();
}

// Game window closed (window thread): same as --run-seconds.
void OnWindowClosed() {
  LogLine("gow2: window closed");
  StopVibrationBeforeExit();
  kernel::stats::WriteSummary();
  StopInstallBeforeExit();
  std::_Exit(0);
}

// xe::FatalError (Xenia code, device lost): after returning it exits with 5.
void OnGpuFatalError() {
  LogLine("gow2: fatal GPU error (exit 5); to start the game without a renderer: --gpu fake");
  kernel::stats::WriteSummary();
}

struct GpuOptions {
  gpu::BackendKind kind = gpu::BackendKind::kD3D12;
  std::vector<gpu::ScreenshotRequest> screenshots;
  bool shader_capture = false;  // --shader-capture
  std::chrono::steady_clock::time_point start;  // startup: base of --screenshot and --input-script
  std::optional<std::vector<hid::ScriptEvent>> input_script;
  plat::WindowOptions window;
  bool stretch = false;
  bool vsync = true;
  uint32_t antialiasing = 0;  // 0 off, 1 fxaa, 2 fxaa_extreme (gpu::D3D12Options)
  bool sharpening = false;
  float sharpness = 0.0f;
  bool enhanced_shaders = true;
  uint32_t upscaler = 0;  // 0 off, 1 fsr, 2 dlss (gpu::D3D12Options)
  settings::DlssPreset dlss_preset = settings::DlssPreset::kDefault;  // [video] dlss_preset
  uint32_t mode_width = 0, mode_height = 0;  // exclusive fullscreen; 0 = current monitor mode
  settings::FrameRate frame_rate = settings::FrameRate::k30;  // [game] frame_rate
  settings::Vrr vrr = settings::Vrr::kAuto;                   // [video] vrr
  settings::FrameGeneration frame_generation = settings::FrameGeneration::kOff;  // [video] frame_generation
  settings::Aspect aspect = settings::Aspect::k16x9;          // [video] aspect
  bool hdr = false, hdr_force = false;                        // [video] hdr; --hdr force (HDR10 even on an SDR output)
  uint32_t hdr_peak_nits = 0, hdr_paper_white_nits = 0;       // [video] hdr_peak_nits, hdr_paper_white_nits (0 = auto)
  std::string dlss_mode_log;  // [gpu] line for the explicit DLSS mode; empty with dlss_mode auto
  // F1 overlay: settings in effect (file + command line) and the file where Apply saves them.
  std::optional<settings::Settings> settings;
  fs::path config;
};

// Result of plat::SdlGamepads::Start, called by wmain before the startup instant.
struct SdlStart {
  std::unique_ptr<plat::SdlGamepads> pads;  // nullptr if SDL did not start
  std::string error;
};

int Run(const fs::path& data_dir, const fs::path& user_data_opt, uint32_t run_seconds, const GpuOptions& gpu_options,
        const apu::AudioOptions& audio_options, std::vector<std::pair<std::string, std::string>> file_replacements,
        SdlStart sdl) {
  const game_profile::Profile& profile = game_profile::Get();
  LogInit(game_profile::LogFileName());

  const fs::path xex = data_dir / "default.xex";
  std::error_code ec;
  if (!fs::is_regular_file(xex, ec)) return Fail(2, "default.xex not found in " + Show(data_dir));
  switch (game_data::CheckXex(data_dir, profile.image_sha256)) {
    case game_data::XexCheck::kOk:
      break;
    case game_data::XexCheck::kMismatch:
      return Fail(2, "default.xex does not match the supported edition (" + std::string(profile.name) + ", " +
                         std::string(profile.edition) + "). Expected image SHA-256 " + std::string(profile.image_sha256) + ".");
    default:
      return Fail(2, "cannot read default.xex in " + Show(data_dir));
  }

  // Audio: opens the WAV capture (if requested) before loading the XEX, the window and
  // the GPU, so a usage error exits with 2 without opening anything. The summary hook is
  // set before the watchdog: every orderly exit goes through WriteSummary, which first
  // stops the XMA decoder and the Audio Worker and closes the WAV; on error exits the
  // guard declared after the heaps stops it (see apu_shutdown).
  if (!apu::Init(audio_options))
    return Fail(2, "cannot open the --audio-capture file " + Show(*audio_options.capture));
  kernel::stats::SetBeforeSummaryHook(&StopAudio);
  struct ApuShutdown {
    ~ApuShutdown() { apu::Shutdown(); }
  };
  // Before the heaps: up to here no audio client is registered, so a Shutdown that
  // only closes the capture is enough.
  ApuShutdown apu_shutdown_early;

  static GuestMemory mem;  // static: the 4 GB arena lives until exit
  if (!mem.Init()) return Fail(1, "cannot reserve 4 GB for guest memory");
  kernel::g_base = mem.base;

  auto image = LoadImage(mem, xex);
  if (!image) return Fail(image.error() == LoadError::FileMissing ? 2 : 1, "loading " + Show(xex) + " failed");
  if (image->base != PPC_IMAGE_BASE || image->size != PPC_IMAGE_SIZE)
    return Fail(1, "the XEX (base " + Hex(image->base) + ", size " + Hex(image->size) +
                       ") does not match the recompiled code (base " + Hex(PPC_IMAGE_BASE) + ", size " +
                       Hex(PPC_IMAGE_SIZE) + "): rerun the recompile target");
  kernel::rtl::SetExecutableSystemFlags(image->system_flags);
  kernel::rtl::SetBeforeExit(StopInstallBeforeExit);  // HalReturnToFirmware (Quit from the game menu), KeBugCheck
  // Game patches: verified and written before any guest thread starts. The two internal
  // frame rate ones (above 30) in the same call, with the same word check.
  std::error_code fs_ec;
  const fs::path data_abs = fs::absolute(data_dir, fs_ec).lexically_normal();
  const fs::path user_data =
      !user_data_opt.empty()
          ? fs::absolute(user_data_opt, fs_ec).lexically_normal()
          : (data_abs.has_filename() ? data_abs : data_abs.parent_path()).parent_path() / profile.user_data_dir;
  // Engine configuration: modified Coalesced files in <user_data>/engine only with a non-default [engine].
  const settings::Engine engine = gpu_options.settings ? gpu_options.settings->engine : settings::Engine{};
  engine_config::Prepared engine_files;
  const bool engine_available = launcher::UnavailableOptions().count("engine") == 0;
  if (engine != settings::Engine{} && engine_available) {
    engine_files = engine_config::Prepare(data_abs / fs::path(profile.cooked_dir).make_preferred(), user_data / "engine", engine);
    for (const auto& line : engine_files.log) LogLine(line);
  }
  settings::FrameRate frame_rate = gpu_options.frame_rate;
  frame_pacing::Pacing pacing = frame_pacing::PacingFor(frame_rate);
  {
    std::set<std::string> patches = gpu_options.settings ? gpu_options.settings->patches : std::set<std::string>{};
    if (pacing.patched) {
      game_patches::SetPresentInterval(pacing.interval);
      patches.insert(game_patches::kFrameRateIntervalKey);
      patches.insert(game_patches::kFrameRateUnlockKey);
    }
    if (!engine_files.replacements.empty()) patches.insert(game_patches::kCoalescedNoHashCheckKey);
    // The aspect ratio entries are internal like the frame rate ones: requested by [video] aspect, not [patches].
    if (gpu_options.aspect == settings::Aspect::k21x9) patches.insert(game_patches::kAspect21x9Key);
    if (gpu_options.aspect == settings::Aspect::k16x10) patches.insert(game_patches::kAspect16x10Key);
    // Motion blur is disabled via the uber post constants (d3d12_backend.cpp), not with the hook: the hook makes
    // the game pick another uber post variant and the upscaler no longer finds the capture and velocity buffer.
    // Without the patch in this game the key stays: Apply logs "not available". A game without the uber post
    // capture in its profile keeps the key: Apply turns on the table's hooks.
    if (profile.post.motion_ps && game_patches::Available(game_patches::kDisableMotionBlur) &&
        patches.erase("disable_motion_blur"))
      LogLine("[patch] disable_motion_blur: turned on (uber post constants)");
    for (const auto& line : game_patches::Apply(mem, patches)) LogLine(line);
  }
  if (pacing.patched && !(game_patches::Enabled(game_patches::kFrameRateInterval) &&
                          game_patches::Enabled(game_patches::kFrameRateUnlock))) {
    // Only one of the two would give combinations never measured: fall back to 30 entirely.
    game_patches::Disable(game_patches::kFrameRateInterval);
    game_patches::Disable(game_patches::kFrameRateUnlock);
    LogLine("[frame] frame rate patches not applied: frame rate 30");
    frame_rate = settings::FrameRate::k30;
    pacing = frame_pacing::PacingFor(frame_rate);
  } else if (pacing.patched) {
    LogLine("[frame] frame rate " + std::string(settings::FrameRateName(frame_rate)) + ": present interval " +
            std::to_string(pacing.interval) + ", Unlock FPS on, vblank " +
            std::to_string(int(pacing.vblank_hz)) + " Hz");
  } else {
    LogLine("[frame] frame rate 30 (original)");
  }
  if (gpu_options.aspect == settings::Aspect::k16x9) {
    LogLine("[aspect] 16:9 (original)");
  } else if (game_patches::Enabled(gpu_options.aspect == settings::Aspect::k21x9 ? game_patches::kAspect21x9
                                                                                   : game_patches::kAspect16x10)) {
    LogLine(gpu_options.aspect == settings::Aspect::k21x9
                ? "[aspect] 21:9: scene 1280x550, MSAA 2x (550-row tiles)"
                : "[aspect] 16:10: scene 1280x800, MSAA 2x (800-row tiles)");
  } else {
    LogLine("[aspect] " + std::string(settings::AspectName(gpu_options.aspect)) +
            " patches not applied: 16:9 aspect");
  }
  if (!gpu_options.dlss_mode_log.empty()) LogLine(gpu_options.dlss_mode_log);
  if (gpu_options.settings && gpu_options.frame_generation != gpu_options.settings->video.frame_generation)
    LogLine("[optiscaler] frame generation turned off: OptiScaler installed");
  if (engine == settings::Engine{}) {
    LogLine("[engine] original configuration");
  } else if (!engine_available) {
    LogLine("[engine] not available for this game");
  } else if (!engine_files.replacements.empty() && !game_patches::Enabled(game_patches::kCoalescedNoHashCheck)) {
    LogLine("[engine] Coalesced check patch not applied: original configuration");
    engine_files.replacements.clear();
  }
  LogLine("image base " + Hex(image->base) + " size " + Hex(image->size) + ", " +
          std::to_string(image->imports.size()) + " import records");

  // Function table right after the image; the heap restarts after the table.
  if (!mem.Commit(uint32_t(kFunctionTableBegin), uint32_t(kFunctionTableEnd - kFunctionTableBegin)))
    return Fail(1, "cannot commit the function table");
  auto table = InstallFunctionTable(mem.base);
  if (!table) return Fail(1, table.error());
  mem.SetHeapStart(uint32_t((kFunctionTableEnd + 0xFFFF) & ~uint64_t(0xFFFF)));
  LogLine("function table " + Hex(kFunctionTableBegin) + "-" + Hex(kFunctionTableEnd) + ": " +
          std::to_string(table->installed) + " functions, " + std::to_string(table->skipped) +
          " skipped; heap from " + Hex(mem.HeapNext()));

  auto imports = ResolveImports(mem, image->imports);
  if (!imports) return Fail(1, imports.error());
  LogLine("imports: " + std::to_string(imports->function_thunks) + " function thunks (__imp__ stubs), " +
          std::to_string(imports->function_slots) + " function slots, " + std::to_string(imports->variables) +
          " variables zeroed");

  // Kernel heaps (virtual, physical, process) on the console's regions.
  if (!kernel::InitMemory(mem)) return Fail(1, "cannot initialize the guest heaps");
  // The heaps must be destroyed before the static arena mem: on leaving Run.
  struct MemoryShutdown {
    ~MemoryShutdown() { kernel::ShutdownMemory(); }
  } memory_shutdown;
  // Destroyed before memory_shutdown: the Audio Worker runs guest code and frees the
  // argument encapsulated in the process heap (apu::Shutdown is idempotent, the
  // earlier guard becomes a no-op).
  ApuShutdown apu_shutdown;
  LogLine("memory: virtual heap " + Hex(kernel::kVirtualHeapBegin) + "-" + Hex(kernel::kVirtualHeapEnd) +
          ", physical " + Hex(kernel::kPhysicalHeapBegin) + "-" + Hex(kernel::kPhysicalHeapEnd) + ", budget " +
          std::to_string(kernel::kBudget >> 20) + " MiB");

  // Executable module (LDR + XEX header) and the kernel's exported variables.
  const uint32_t hmodule = kernel::module::InitExecutable(
      {image->base, image->size, image->entry, image->header, "default.xex"});
  if (!hmodule) return Fail(1, "cannot prepare the LDR of the executable module");
  kernel::module::SetProcedureInstaller(
      [](kernel::module::HostFunction* f) { return InstallExtraFunction(kernel::g_base, f); });
  // The KeTimeStampBundle ticker (a host thread writing into the arena) must be
  // stopped on every error exit before the heaps and the static arena: declared
  // after memory_shutdown, it is destroyed first.
  struct KernelVariablesShutdown {
    ~KernelVariablesShutdown() { kernel::module::StopKernelVariables(); }
  } kernel_variables_shutdown;
  {
    std::string names;
    for (const auto& n : kernel::module::InitKernelVariables(image->imports)) names += " " + n;
    LogLine("module default.xex: hmodule " + Hex(hmodule) + ", XEX header at " +
            Hex(kernel::module::ExecutableHeader()) + "; kernel variables initialized:" + names);
  }

  // Guest file system: read-only disc, writable cache.
  {
    const fs::path cache_dir = user_data / "cache";
    fs::create_directories(cache_dir, fs_ec);
    if (fs_ec) return Fail(1, "cannot create " + Show(cache_dir));
    const fs::path content_dir = user_data / "content";
    fs::create_directories(content_dir, fs_ec);
    if (fs_ec) return Fail(1, "cannot create " + Show(content_dir));
    kernel::xam::SetContentRoot(content_dir);
    kernel::io::SetMounts({{"game:", data_abs, false},
                           {"d:", data_abs, false},
                           {"\\Device\\Cdrom0", data_abs, false},
                           {"cache:", cache_dir, true}});
    for (auto& r : engine_files.replacements) file_replacements.push_back(std::move(r));
    kernel::io::SetFileReplacements(std::move(file_replacements));
    LogLine("file system: game:, d:, \\Device\\Cdrom0 -> " + Show(data_abs) + " (read-only); cache: -> " +
            Show(cache_dir) + "; XAM content in " + Show(content_dir));
  }

  // The watchdog is detached and sleeps until the deadline: on error exits the
  // process ends before it wakes up.
  if (run_seconds) StartWatchdog(run_seconds);

  CrashMapInit();
  // Physical page watching for GPU invalidation: its vectored write handler runs
  // before the crash handler, which still receives every unobserved access violation.
  physwatch::Init(mem);
  plat::InstallCrashHandler(&OnCrash);

  // Guest functions from the function table: thread start, APCs, callbacks.
  if (!kernel::InitThreads(
          mem, [](uint32_t guest) { return LookupFunction(kernel::g_base, guest); }, image->default_stack_size))
    return Fail(1, "cannot initialize the guest threads");

  // GPU: registers 0x7FC80000-0x7FCFFFFF and vblank (common module), the backend's
  // command processor (guest thread with a host body). Stopped before the heaps on
  // error exits; the normal exit is _Exit. With d3d12 the window (declared first:
  // destroyed after the backend, which uses its swap chain) and the D3D12 device.
  gpu::SetFatalErrorHook(&OnGpuFatalError);
  struct WindowGuard {
    plat::Window* window = nullptr;
    ~WindowGuard() { plat::DestroyGameWindow(window); }
  } game_window;
  std::unique_ptr<gpu::Backend> gpu_backend;
  switch (gpu_options.kind) {
    case gpu::BackendKind::kFake:
      gpu_backend = gpu_fake::CreateBackend();
      break;
    case gpu::BackendKind::kD3D12: {
      game_window.window = plat::CreateGameWindow(std::string(profile.name) + " (" + std::string(profile.id) + ")", gpu_options.window, true, &OnWindowClosed);
      if (!game_window.window) return Fail(1, "cannot create the game window");
      gpu::D3D12Options options;
      options.window = game_window.window;
      options.screenshots = gpu_options.screenshots;
      options.shader_capture = gpu_options.shader_capture;
      options.start = gpu_options.start;
      options.stretch = gpu_options.stretch;
      options.vsync = gpu_options.vsync;
      options.antialiasing = gpu_options.antialiasing;
      options.sharpening = gpu_options.sharpening;
      options.sharpness = gpu_options.sharpness;
      options.enhanced_shaders = gpu_options.enhanced_shaders;
      options.upscaler = gpu_options.upscaler;
      options.dlss_preset = gpu_options.dlss_preset;
      options.settings = gpu_options.settings;
      options.config = gpu_options.config;
      options.exclusive_fullscreen = gpu_options.window.mode == plat::WindowMode::kFullscreen;
      options.mode_width = gpu_options.mode_width;
      options.mode_height = gpu_options.mode_height;
      options.frame_rate = frame_rate;
      options.vrr = gpu_options.vrr;
      options.frame_generation = gpu_options.frame_generation;
      options.hdr = gpu_options.hdr;
      options.hdr_force = gpu_options.hdr_force;
      options.hdr_peak_nits = gpu_options.hdr_peak_nits;
      options.hdr_paper_white_nits = gpu_options.hdr_paper_white_nits;
      options.shader_cache_root = user_data;
      options.title_id = kernel::module::ExecutableTitleId();
      gpu_backend = gpu::CreateD3D12Backend(mem, options);
      if (!gpu_backend) {
        LogLine("gow2: cannot create the D3D12 device (see the [gpu] and [xenia] lines above); "
                "to start the game without a renderer: --gpu fake");
        kernel::stats::WriteSummary();
        return 5;
      }
      break;
    }
  }
  gpu::SetVblankPeriod(frame_pacing::VblankPeriod(pacing.vblank_hz));
  if (!gpu::Init(mem, std::move(gpu_backend))) return Fail(1, "cannot initialize the GPU");
  gpu::InitVideoVariables(image->imports);
  struct GpuShutdown {
    ~GpuShutdown() { gpu::Shutdown(); }
  } gpu_shutdown;

  // XMA decoder: registers 0x7FEA0000-0x7FEAFFFF and contexts (kernel::xma), then one
  // xenia-canary XmaContextNew (or Old) per context and the "XMA Decoder" worker
  // (apu/xma_decoder.h), before the game allocates contexts.
  if (!kernel::xma::Init(mem)) return Fail(1, "cannot initialize the XMA registers");
  if (!apu::StartXmaDecoder(mem, {}, audio_options.xma_decoder)) return Fail(1, "cannot start the XMA decoder");
  struct XmaDecoderShutdown {
    ~XmaDecoderShutdown() { apu::StopXmaDecoder(); }
  } xma_decoder_shutdown;
  LogLine("xma: registers " + Hex(kernel::xma::kRegisterBase) + "-" +
          Hex(kernel::xma::kRegisterBase + kernel::xma::kRegisterRegionSize - 1) + ", " +
          std::to_string(kernel::xma::kContextCount) + " contexts at " + Hex(kernel::xma::ContextArray()) +
          ", XMA decoder " + apu::XmaDecoderName(audio_options.xma_decoder) + " started");

  // Input: script (only with --input-script), XInput, SDL pads (non-Xbox), keyboard (always
  // last: virtual pad 0 is connected even without an XInput pad).
  {
    std::vector<std::unique_ptr<hid::InputDriver>> drivers;
    std::string names;
    if (gpu_options.input_script) {
      drivers.push_back(hid::CreateScriptDriver(*gpu_options.input_script, gpu_options.start));
      names += " script(" + std::to_string(gpu_options.input_script->size()) + " events)";
    }
    // Non-owning pointer to the XInput driver: the SDL driver uses it to yield
    // slots (GetState reads the seqlock cache, with no OS calls or locks). Alive
    // until hid::Init/Shutdown destroys the drivers; the SDL driver calls the callback
    // only inside GetState (under the InputSystem lock) and Init destroys them
    // outside the lock, after removing them from the Poll loop: the destruction
    // order between the two does not matter.
    hid::InputDriver* xinput_raw = nullptr;
    if (auto xinput = hid::CreateXInputDriver()) {
      xinput_raw = xinput.get();
      drivers.push_back(std::move(xinput));
      names += " xinput";
    } else {
      names += " (xinput1_4.dll missing)";
    }
    // SDL has already started in wmain (see SdlStart).
    if (sdl.pads) {
      drivers.push_back(hid::CreateSdlDriver(
          hid::CreateSdlPadSource(std::move(sdl.pads)),
          [xinput_raw](uint32_t user) { return xinput_raw && xinput_raw->GetState(user).has_value(); }));
      names += " sdl";
    } else {
      LogLine("[input] SDL unavailable: " + sdl.error);
    }
    drivers.push_back(hid::CreateKeyboardDriver(game_window.window));
    names += game_window.window ? " keyboard" : " keyboard(windowless)";
    hid::Init(std::move(drivers));
    LogLine("input: driver" + names);
  }
  struct HidShutdown {
    ~HidShutdown() { hid::Shutdown(); }
  } hid_shutdown;

  if (!LookupFunction(mem.base, image->entry))
    return Fail(1, "entry point " + Hex(image->entry) + " has no recompiled function");

  // The main thread is a GuestThread like the others (PCR, KTHREAD, TLS, guarded
  // stack); this host thread waits for it to finish.
  LogLine("entry " + Hex(image->entry));
  auto main_thread = kernel::GuestThread::Create(image->entry, 0, kStackSize, false);
  if (!main_thread) return Fail(1, "guest space exhausted for the main thread");
  const std::shared_ptr<kernel::KernelObject> wait_main[1] = {main_thread};
  kernel::WaitAny(wait_main, nullptr, false);
  LogLine("entry returned, r3=" + Hex(main_thread->ExitCode()));
  StopVibrationBeforeExit();
  kernel::stats::WriteSummary();
  StopInstallBeforeExit();
  // Other guest threads may still be alive: no static destructors (guest arena,
  // heaps) under their feet.
  std::_Exit(0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // As early as possible so usage/argument errors reach a terminal; upgraded once gow2.toml is read.
  plat::SetupConsole(false);
  // Before any window: sizes in physical pixels, no blurry scaling.
  plat::EnablePerMonitorDpiAwareness();
  // gow2.toml next to the exe or --config: read below, after the launcher if any.
  fs::path config = plat::ExecutablePath().parent_path() / game_profile::SettingsFileName();
  bool explicit_config = false;
  // GOW2_CONFIG like --config (which overrides it): the tests point it at an empty file.
  {
    wchar_t* env = nullptr;
    size_t env_len = 0;
    if (_wdupenv_s(&env, &env_len, L"GOW2_CONFIG") == 0 && env && *env) {
      config = fs::path(env);
      explicit_config = true;
    }
    std::free(env);
  }
  bool want_launcher = argc == 1;  // double click: no arguments
  bool help = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view a = argv[i];
    if (a == L"--config" && i + 1 < argc) {
      config = fs::path(argv[i + 1]);
      explicit_config = true;
    } else if (a == L"--launcher") {
      want_launcher = true;
    } else if (a == L"--help") {
      help = true;
    }
  }
  // Launcher before SDL and the startup instant: the time spent in the settings does
  // not shift --screenshot and --input-script. If it cannot start the game runs with the file as it is.
  if (want_launcher && !help) {
    // A dxgi.dll proxy next to the exe (OptiScaler) is for the game, not the launcher: dxgi and d3d11 are
    // delay-loaded, so the launcher takes the system ones; the game then restarts in a fresh process.
    const fs::path proxy = launcher::ExeDir() / "dxgi.dll";
    std::error_code ec;
    if (fs::exists(proxy, ec)) SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::string why;
    switch (launcher::Run(config, {}, &why)) {
      case launcher::Result::kQuit:
        return 0;
      case launcher::Result::kError:
        ErrPuts("launcher unavailable: " + why + "\n");
        break;
      case launcher::Result::kPlay:
        break;
    }
    // Also a proxy installed from the launcher. --config skips the launcher (Play has saved the file).
    if (fs::exists(proxy, ec) && !optiscaler::Loaded(launcher::ExeDir())) {
      // shortcut: plain quoting, breaks on an argument ending in a backslash; upgrade if such paths appear.
      std::wstring cmd = L"\"" + plat::ExecutablePath().wstring() + L"\"";
      for (int i = 1; i < argc; ++i)
        if (std::wstring_view(argv[i]) != L"--launcher") cmd += L" \"" + std::wstring(argv[i]) + L"\"";
      cmd += L" --config \"" + config.wstring() + L"\"";
      STARTUPINFOW si{sizeof si};
      PROCESS_INFORMATION pi{};
      if (!CreateProcessW(plat::ExecutablePath().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &si, &pi)) {
        ErrPuts("restarting the game failed (error " + std::to_string(GetLastError()) + ")\n");
        return 1;
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return 0;
    }
  }
  // As the static import did before the delay load: a dxgi.dll proxy loads at startup, before the GPU.
  LoadLibraryW(L"dxgi.dll");
  // Default: the profile's data folder (game_data / game_data_gow3) in the current directory; if it does not exist, next to the executable.
  fs::path data_dir = game_data::ResolveDataDir(plat::ExecutablePath().parent_path(), game_profile::Get().data_dir);
  fs::path user_data_dir;  // empty: <parent of data>/<profile user_data folder>
  uint32_t run_seconds = 0;
  // SDL_Init (HIDAPI and pad enumeration) takes a variable amount of time: done
  // here, synchronously and before taking the startup instant, it does not shift the
  // game relative to the clock of --screenshot and --input-script. The result
  // goes to Run, which writes it to the log and creates the driver.
  SdlStart sdl;
  sdl.pads = plat::SdlGamepads::Start(&sdl.error);
  GpuOptions gpu_options;
  gpu_options.start = std::chrono::steady_clock::now();
  bool gpu_probe = false;
  std::vector<std::pair<std::string, std::string>> skip_movies;
  apu::AudioOptions audio_options;
  // gow2 defaults for Xenia's GPU code (Global Constraints), before the user's
  // --gpu-cvar.
  if (!gpu::SetXeniaCvar("render_target_path_d3d12", "rtv")) {
    ErrPuts("cvar render_target_path_d3d12 missing: incomplete build\n");
    return 1;
  }
  // tfetch offsets in guest texels even at internal resolution > 1: UE3's 4x4 depth of field
  // downsample with offsets in host texels read only the block's corner and shifted the
  // blurred layer (double silhouettes at 3x). In the game only the downsamples use the offsets.
  if (!gpu::SetXeniaCvar("draw_resolution_scaled_texture_offsets", "false")) {
    ErrPuts("cvar draw_resolution_scaled_texture_offsets missing: incomplete build\n");
    return 1;
  }
  // Occlusion queries with true counts (waits for the GPU when the game waits for a result): in
  // "fast" Xenia writes an estimate immediately (the last count or 1 sample) and corrects it later; with the GPU
  // 2-3 frames behind the game reads the estimate, believes the soldiers are almost invisible and switches to another
  // material (blue armor at 2x/3x, even at 1x with FSR or a shared GPU). Cost: ~12% at 1x with unlimited
  // fps; at 2x/3x faster than "fast" with a single frame queued.
  if (!gpu::SetXeniaCvar("occlusion_query", "strict")) {
    ErrPuts("cvar occlusion_query missing: incomplete build\n");
    return 1;
  }
  // Once every 2 frames the game waits (spinning) for the result of a query just submitted: with
  // one submission every 100 draws the GPU has already almost executed it when the command processor runs out
  // of commands, instead of only starting then. Only at internal resolution 1x (patch 0016): from ~115 to ~144
  // fps; at 2x/3x the GPU is saturated and splitting would bring the blue armor back.
  if (!gpu::SetXeniaCvar("d3d12_submit_every_draws", "100")) {
    ErrPuts("cvar d3d12_submit_every_draws missing: incomplete build\n");
    return 1;
  }
  // GoW3 at 2x/3x: the GPU is idle ~30% of the frame waiting for the CPU at the query wait; splitting there too
  // overlaps them (3x gameplay: 79 -> 117 fps) with the counts unchanged (per-frame ZPD statistics).
  if (!gpu::SetXeniaCvar("d3d12_submit_every_draws_scaled", game_profile::Get().submit_split_scaled ? "true" : "false")) {
    ErrPuts("cvar d3d12_submit_every_draws_scaled missing: incomplete build\n");
    return 1;
  }
  // The XDK Clear draws at 4x on the depth owned by a 1x target: remapped to the owner's MSAA,
  // no EDRAM round-trip transfers, 78% of those in a frame (patch 0017).
  if (!gpu::SetXeniaCvar("gow2_clear_msaa_remap", "true")) {
    ErrPuts("cvar gow2_clear_msaa_remap missing: incomplete build\n");
    return 1;
  }
  // At internal resolution > 1 the depth of the scene's second band (rows 512-719) must not be
  // stretched over the first row: it would leave a dark line on the characters at row 512 (patch 0018).
  if (!gpu::SetXeniaCvar("gow2_resolve_fill_depth", "false")) {
    ErrPuts("cvar gow2_resolve_fill_depth missing: incomplete build\n");
    return 1;
  }
  // Half-resolution sun light packs the values of its 2x2 pixels into each texel and the pass that
  // applies it picks the value on the guest grid: scaled, every pixel took its neighbor's (blocks
  // against the sky, missing haze). Surfaces up to 640 px wide stay native.
  if (!gpu::SetXeniaCvar("draw_resolution_scale_threshold", "640")) {
    ErrPuts("cvar draw_resolution_scale_threshold missing: incomplete build\n");
    return 1;
  }
  // gow2.toml (or --config) before the other options: the command line
  // overrides it, its cvars precede --gpu-cvar.
  settings::Settings cfg;
  bool cli_internal_resolution_given = false;  // for the DLSS mode line
  {
    uint32_t cli_internal_resolution = 0;  // 0 = not passed (an invalid value is reported by the loop below)
    std::optional<uint32_t> cli_anisotropic;  // same (here 0 is a valid value)
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::wstring_view(argv[i]) == L"--internal-resolution") {
        const std::wstring_view v = argv[i + 1];
        if (v == L"1" || v == L"2" || v == L"3") cli_internal_resolution = uint32_t(v[0] - L'0');
      }
      if (std::wstring_view(argv[i]) == L"--anisotropic") {
        const std::wstring_view v = argv[i + 1];
        for (const uint32_t n : {0u, 2u, 4u, 8u, 16u})
          if (v == std::to_wstring(n)) cli_anisotropic = n;
      }
    }
    std::error_code ec;
    const std::u8string cfg_path_u8 = config.u8string();
    if (explicit_config && !fs::exists(config, ec)) {
      ErrPuts(("--config: cannot read the file " + std::string(cfg_path_u8.begin(), cfg_path_u8.end()) + "\n").c_str());
      return 2;
    }
    if (const auto err = settings::LoadFile(config, cfg)) {
      ErrPuts((*err + "\n").c_str());
      return 2;
    }
    plat::SetupConsole(cfg.console);
    // [game] language before the options loop: --language overrides it.
    if (!cfg.language.empty()) kernel::xam::SetLanguage(*kernel::xam::LanguageFromCode(cfg.language));
    // Precedence: file internal_resolution < --internal-resolution < [gpu].cvars < --gpu-cvar.
    if (cli_internal_resolution) cfg.video.internal_resolution = cli_internal_resolution;
    if (cfg.video.internal_resolution > 1) {
      const std::string n = std::to_string(cfg.video.internal_resolution);
      if (!gpu::SetXeniaCvar("draw_resolution_scale_x", n) || !gpu::SetXeniaCvar("draw_resolution_scale_y", n)) {
        ErrPuts("internal_resolution: cvar draw_resolution_scale not available\n");
        return 2;
      }
    }
    // Precedence: file anisotropic < --anisotropic < [gpu].cvars < --gpu-cvar. 0 = no override.
    if (cli_anisotropic) cfg.video.anisotropic = *cli_anisotropic;
    if (const int32_t aniso = settings::AnisotropicOverride(cfg.video.anisotropic); aniso >= 0) {
      if (!gpu::SetXeniaCvar("anisotropic_override", std::to_string(aniso))) {
        ErrPuts("anisotropic: cvar anisotropic_override not available\n");
        return 2;
      }
    }
    for (const auto& [name, value] : cfg.gpu_cvars) {
      if (!gpu::SetXeniaCvar(name, value)) {
        const std::u8string cfg_name = config.filename().u8string();
        ErrPuts((std::string(cfg_name.begin(), cfg_name.end()) + ": [gpu].cvars: " + name + "=" + value +
                    ": nonexistent Xenia GPU cvar or invalid value\n")
                       .c_str());
        return 2;
      }
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--config" && i + 1 < argc) {
      ++i;  // already read above
    } else if (arg == L"--window" && i + 1 < argc) {
      const std::string name = Narrow(argv[++i]);
      const auto mode = settings::ParseWindowMode(name);
      if (!mode) {
        ErrPuts(("--window invalid: " + name + " (windowed|borderless|fullscreen)\n").c_str());
        return 2;
      }
      cfg.video.mode = *mode;
    } else if (arg == L"--resolution" && i + 1 < argc) {
      const std::string res = Narrow(argv[++i]);
      if (!settings::ParseResolution(res, cfg.video.width, cfg.video.height)) {
        ErrPuts(("--resolution invalid: " + res + " (format WxH, 320..7680 x 240..4320)\n").c_str());
        return 2;
      }
    } else if (arg == L"--vsync" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "on" && v != "off") {
        ErrPuts(("--vsync invalid: " + v + " (on|off)\n").c_str());
        return 2;
      }
      cfg.video.vsync = v == "on";
    } else if (arg == L"--frame-rate" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseFrameRate(v);
      if (!r) {
        ErrPuts(("--frame-rate invalid: " + v + " (allowed values: 30, 60, 120, 144, monitor, unlimited)\n").c_str());
        return 2;
      }
      cfg.frame_rate = *r;
    } else if (arg == L"--vrr" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseVrr(v);
      if (!r) {
        ErrPuts(("--vrr invalid: " + v + " (auto|on|off)\n").c_str());
        return 2;
      }
      cfg.video.vrr = *r;
    } else if (arg == L"--frame-generation" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      // "on" stays as an alias of "fsr" (old behavior).
      const auto r = settings::ParseFrameGeneration(v == "on" ? "fsr" : v);
      if (!r) {
        ErrPuts(("--frame-generation invalid: " + v + " (off|fsr|dlss)\n").c_str());
        return 2;
      }
      cfg.video.frame_generation = *r;
    } else if (arg == L"--hdr" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "on" && v != "off" && v != "force") {
        ErrPuts(("--hdr invalid: " + v + " (on|off|force)\n").c_str());
        return 2;
      }
      if (v != "force") cfg.video.hdr = v == "on";  // force does not touch cfg: the F1 overlay would save hdr = true
      gpu_options.hdr_force = v == "force";  // for this run only: it does not go through the file
    } else if ((arg == L"--hdr-peak-nits" || arg == L"--hdr-paper-white-nits") && i + 1 < argc) {
      const bool peak = arg == L"--hdr-peak-nits";
      const std::string v = Narrow(argv[++i]);
      const uint32_t lo = peak ? settings::kHdrPeakNitsMin : settings::kHdrPaperWhiteNitsMin;
      const uint32_t hi = peak ? settings::kHdrPeakNitsMax : settings::kHdrPaperWhiteNitsMax;
      unsigned long n = 0;
      const bool digits = !v.empty() && v.size() <= 5 && v.find_first_not_of("0123456789") == std::string::npos;
      if (digits) n = std::stoul(v);
      if (!digits || (n != 0 && (n < lo || n > hi))) {
        ErrPuts(((peak ? "--hdr-peak-nits invalid: " : "--hdr-paper-white-nits invalid: ") + v + " (0 or " +
                    std::to_string(lo) + "-" + std::to_string(hi) + ")\n").c_str());
        return 2;
      }
      (peak ? cfg.video.hdr_peak_nits : cfg.video.hdr_paper_white_nits) = uint32_t(n);
    } else if (arg == L"--dlss-preset" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseDlssPreset(v);
      if (!r) {
        ErrPuts(("--dlss-preset invalid: " + v + " (default|e|f|j|k|l|m)\n").c_str());
        return 2;
      }
      cfg.video.dlss_preset = *r;
    } else if (arg == L"--dlss-mode" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseDlssMode(v);
      if (!r) {
        ErrPuts(("--dlss-mode invalid: " + v + " (auto|dlaa|quality|balanced|performance|ultra_performance)\n")
                       .c_str());
        return 2;
      }
      cfg.video.dlss_mode = *r;
    } else if (arg == L"--aspect" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseAspect(v);
      if (!r) {
        ErrPuts(("--aspect invalid: " + v + " (16:9|21:9|16:10)\n").c_str());
        return 2;
      }
      cfg.video.aspect = *r;
    } else if (arg == L"--shadow-quality" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto r = settings::ParseShadowQuality(v);
      if (!r) {
        ErrPuts(("--shadow-quality invalid: " + v + " (low|original|high)\n").c_str());
        return 2;
      }
      cfg.engine.shadow_quality = *r;
    } else if (arg == L"--dynamic-shadows" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "on" && v != "off") {
        ErrPuts(("--dynamic-shadows invalid: " + v + " (on|off)\n").c_str());
        return 2;
      }
      cfg.engine.dynamic_shadows = v == "on";
    } else if (arg == L"--aa" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto aa = settings::ParseAntialiasing(v);
      if (!aa) {
        ErrPuts(("--aa invalid: " + v + " (off|fxaa|fxaa_extreme)\n").c_str());
        return 2;
      }
      cfg.video.antialiasing = *aa;
    } else if (arg == L"--upscaler" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      const auto u = settings::ParseUpscaler(v);
      if (!u) {
        ErrPuts(("--upscaler invalid: " + v + " (off|fsr|dlss)\n").c_str());
        return 2;
      }
      cfg.video.upscaler = *u;
    } else if (arg == L"--sharpening" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      char* end = nullptr;
      const double d = std::strtod(v.c_str(), &end);
      if (v == "off") {
        cfg.video.sharpening = false;
      } else if (v.empty() || end != v.c_str() + v.size() || !(d >= 0.0 && d <= 1.0)) {
        ErrPuts(("--sharpening invalid: " + v + " (off or 0.0..1.0)\n").c_str());
        return 2;
      } else {
        cfg.video.sharpening = true;
        cfg.video.sharpness = float(d);
      }
    } else if (arg == L"--enhanced-shaders" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "on" && v != "off") {
        ErrPuts(("--enhanced-shaders invalid: " + v + " (on|off)\n").c_str());
        return 2;
      }
      cfg.video.enhanced_shaders = v == "on";
    } else if (arg == L"--skip-movie" && i + 1 < argc) {
      const std::string name = Narrow(argv[++i]) + ".bik";
      if (!kernel::io::IsValidComponent(name)) {
        ErrPuts(("--skip-movie invalid: " + Narrow(argv[i]) + " (name of a .bik file without the extension)\n").c_str());
        return 2;
      }
      skip_movies.emplace_back(name, "LoadingMovie.bik");  // the shortest one on the disc
    } else if (arg == L"--internal-resolution" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "1" && v != "2" && v != "3") {  // same as internal_resolution in settings.cpp
        ErrPuts(("--internal-resolution invalid: " + v + " (outside the range 1..3)\n").c_str());
        return 2;
      }
      cfg.video.internal_resolution = uint32_t(v[0] - '0');
      cli_internal_resolution_given = true;
    } else if (arg == L"--anisotropic" && i + 1 < argc) {
      const std::string v = Narrow(argv[++i]);
      if (v != "0" && v != "2" && v != "4" && v != "8" && v != "16") {  // same as anisotropic in settings.cpp
        ErrPuts(("--anisotropic invalid: " + v + " (allowed values: 0, 2, 4, 8, 16)\n").c_str());
        return 2;
      }
      cfg.video.anisotropic = uint32_t(std::stoul(v));
    } else if (arg == L"--data" && i + 1 < argc) {
      data_dir = fs::path(argv[++i]);
    } else if (arg == L"--user-data" && i + 1 < argc) {
      user_data_dir = fs::path(argv[++i]);
    } else if (arg == L"--run-seconds" && i + 1 < argc) {
      wchar_t* end = nullptr;
      const unsigned long n = std::wcstoul(argv[++i], &end, 10);
      if (end == argv[i] || *end || n == 0 || n > 86400) {
        ErrPuts("--run-seconds requires an integer between 1 and 86400\n");
        return 2;
      }
      run_seconds = uint32_t(n);
    } else if (arg == L"--language" && i + 1 < argc) {
      std::string code;
      for (wchar_t c : std::wstring_view(argv[++i])) code.push_back(c < 0x80 ? char(c) : '?');
      const auto language = kernel::xam::LanguageFromCode(code);
      if (!language) {
        ErrPuts(("unknown language: " + code + " (allowed: en ja de fr es it ko zh zh-cn pt pl ru)\n").c_str());
        return 2;
      }
      kernel::xam::SetLanguage(*language);
    } else if (arg == L"--gpu" && i + 1 < argc) {
      std::string name;
      for (wchar_t c : std::wstring_view(argv[++i])) name.push_back(c < 0x80 ? char(c) : '?');
      const auto kind = gpu::ParseBackendKind(name);
      if (!kind) {
        ErrPuts(("unknown GPU backend: " + name + " (allowed: fake d3d12)\n").c_str());
        return 2;
      }
      gpu_options.kind = *kind;
    } else if (arg == L"--screenshot" && i + 1 < argc) {
      const auto shot = gpu::ParseScreenshotArg(argv[++i]);
      if (!shot) {
        ErrPuts(("--screenshot invalid: " + Narrow(argv[i]) + " (format S:file, S between 0 and 86400)\n").c_str());
        return 2;
      }
      gpu_options.screenshots.push_back(*shot);
    } else if (arg == L"--shader-capture") {
      gpu_options.shader_capture = true;
    } else if (arg == L"--input-script" && i + 1 < argc) {
      const std::string script = Narrow(argv[++i]);
      gpu_options.input_script = hid::ParseInputScript(script);
      if (!gpu_options.input_script) {
        ErrPuts(("--input-script invalid: " + script +
                    " (format <s>:<BUTTON>[+<ms>],..., BUTTON one of DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT "
                    "START BACK LTHUMB RTHUMB LB RB A B X Y LS_UP LS_DOWN LS_LEFT LS_RIGHT LT RT)\n")
                       .c_str());
        return 2;
      }
    } else if (arg == L"--gpu-cvar" && i + 1 < argc) {
      const std::string setting = Narrow(argv[++i]);
      const size_t eq = setting.find('=');
      if (eq == std::string::npos || eq == 0 ||
          !gpu::SetXeniaCvar(std::string_view(setting).substr(0, eq), std::string_view(setting).substr(eq + 1))) {
        ErrPuts(("--gpu-cvar invalid: " + setting +
                    " (format name=value: existing Xenia GPU cvar, value of its type)\n")
                       .c_str());
        return 2;
      }
    } else if (arg == L"--gpu-probe") {
      gpu_probe = true;
    } else if (arg == L"--audio" && i + 1 < argc) {
      const std::string name = Narrow(argv[++i]);
      const auto sink = apu::ParseAudioSink(name);
      if (!sink) {
        ErrPuts(("--audio invalid: " + name + " (xaudio2|null)\n").c_str());
        return 2;
      }
      audio_options.sink = *sink;
    } else if (arg == L"--xma-decoder" && i + 1 < argc) {
      const std::string name = Narrow(argv[++i]);
      const auto kind = apu::ParseXmaDecoder(name);
      if (!kind) {
        ErrPuts(("--xma-decoder invalid: " + name + " (new|old)\n").c_str());
        return 2;
      }
      audio_options.xma_decoder = *kind;
    } else if (arg == L"--mute") {
      audio_options.mute = true;
    } else if (arg == L"--audio-capture" && i + 1 < argc) {
      audio_options.capture = fs::path(argv[++i]);
    } else if (arg == L"--launcher") {
      // already handled at startup
    } else if (arg == L"--trace") {
      kernel::stats::SetTrace(true);
    } else {
      ErrPuts("usage: gow2 [--data <folder with default.xex>] [--user-data <folder>] [--trace] [--run-seconds N] "
                 "[--language <code>] [--gpu fake|d3d12] [--screenshot S:file] [--gpu-cvar name=value]... "
                 "[--gpu-probe] [--shader-capture] [--audio xaudio2|null] [--mute] [--audio-capture <file.wav>] [--xma-decoder new|old] "
                 "[--input-script \"<s>:<BUTTON>[+<ms>],...\"] [--config <file>] "
                 "[--window windowed|borderless|fullscreen] [--resolution WxH] [--vsync on|off] "
                 "[--frame-rate 30|60|120|144|monitor|unlimited] [--vrr auto|on|off] "
                 "[--frame-generation off|fsr|dlss] [--aspect 16:9|21:9|16:10] [--hdr on|off|force] [--hdr-peak-nits <n>] [--hdr-paper-white-nits <n>] [--shadow-quality low|original|high] [--dynamic-shadows on|off] "
                 "[--aa off|fxaa|fxaa_extreme] [--sharpening off|0.0..1.0] [--internal-resolution 1|2|3] "
                 "[--anisotropic 0|2|4|8|16] [--enhanced-shaders on|off] [--upscaler off|fsr|dlss] [--dlss-preset default|e|f|j|k|l|m] [--dlss-mode auto|dlaa|quality|balanced|performance|ultra_performance] [--skip-movie <name>]... [--launcher]\n");
      return 2;
    }
  }
  // Explicit DLSS mode: decides the internal resolution and wins over the file, --internal-resolution and scale cvars.
  // cfg.video.internal_resolution stays the one chosen by the user: Apply in the F1 overlay saves it as it is.
  // Unreachable (e.g. monitor changed): the game plays with the internal resolution already applied.
  if (cfg.video.upscaler == settings::Upscaler::kDlss && cfg.video.dlss_mode != settings::DlssMode::kAuto) {
    const auto [desktop_w, desktop_h] = plat::PrimaryMonitorSize();
    const auto [out_w, out_h] = settings::OutputSizeFor(cfg.video, desktop_w, desktop_h);
    const std::string mode(settings::DlssModeName(cfg.video.dlss_mode));
    const std::string out = std::to_string(out_w) + "x" + std::to_string(out_h);
    if (const auto n = settings::InternalResolutionForDlssMode(cfg.video.dlss_mode,
                                                               settings::ImageWidthFor(cfg.video, out_w, out_h))) {
      const std::string ns = std::to_string(*n);
      if (!gpu::SetXeniaCvar("draw_resolution_scale_x", ns) || !gpu::SetXeniaCvar("draw_resolution_scale_y", ns)) {
        ErrPuts("dlss_mode: cvar draw_resolution_scale not available\n");
        return 2;
      }
      gpu_options.dlss_mode_log = "[gpu] DLSS: mode " + mode + " -> internal resolution " + ns + "x (output " +
                                  out + ")" + (cli_internal_resolution_given ? ", --internal-resolution ignored" : "");
    } else {
      gpu_options.dlss_mode_log = "[gpu] DLSS: mode " + mode + " not reachable at " + out +
                                  ", using internal resolution " + std::to_string(cfg.video.internal_resolution) + "x";
    }
  }
  // width/height from the file: client area in windowed, video mode in fullscreen;
  // in borderless the window covers the monitor. Alt-Enter returns to 1280x720.
  {
    const auto ws = launcher::WindowSetupFor(cfg.video);
    gpu_options.window = ws.window;
    gpu_options.mode_width = ws.mode_width;
    gpu_options.mode_height = ws.mode_height;
  }
  gpu_options.stretch = cfg.video.scaling == settings::Scaling::kStretch;
  gpu_options.vsync = cfg.video.vsync;
  gpu_options.antialiasing = uint32_t(cfg.video.antialiasing);
  gpu_options.sharpening = cfg.video.sharpening;
  gpu_options.sharpness = cfg.video.sharpness;
  gpu_options.enhanced_shaders = cfg.video.enhanced_shaders;
  gpu_options.upscaler = uint32_t(cfg.video.upscaler);
  gpu_options.dlss_preset = cfg.video.dlss_preset;
  gpu_options.frame_rate = cfg.frame_rate;
  gpu_options.vrr = cfg.video.vrr;
  gpu_options.frame_generation = optiscaler::EffectiveFrameGeneration(cfg.video.frame_generation,
                                                                      optiscaler::Detect(launcher::ExeDir()));
  gpu_options.aspect = cfg.video.aspect;
  gpu_options.hdr = cfg.video.hdr || gpu_options.hdr_force;
  gpu_options.hdr_peak_nits = cfg.video.hdr_peak_nits;
  gpu_options.hdr_paper_white_nits = cfg.video.hdr_paper_white_nits;
  gpu_options.settings = cfg;
  gpu_options.config = config;
  if (gpu_probe) {
    // Adapter only: the game does not start (no log and no summary).
    const auto adapter = gpu::FindHardwareAdapter();
    if (!adapter) {
      ErrPuts("no hardware D3D12 adapter (feature level 11_0)\n");
      return 5;
    }
    std::fputs((*adapter + "\n").c_str(), stdout);
    return 0;
  }
  if (!gpu_options.screenshots.empty() && gpu_options.kind != gpu::BackendKind::kD3D12) {
    ErrPuts("--screenshot invalid with --gpu fake (no presenter)\n");
    return 2;
  }
  if (gpu_options.shader_capture && gpu_options.kind != gpu::BackendKind::kD3D12) {
    ErrPuts("--shader-capture invalid with --gpu fake (no presenter)\n");
    return 2;
  }
  return Run(data_dir, user_data_dir, run_seconds, gpu_options, audio_options, std::move(skip_movies),
             std::move(sdl));
}
