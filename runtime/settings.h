// gow2 settings read from gow2.toml (toml++) and from the command line.
// Precedence: default < gow2.toml < command line (applied by main.cpp).
// No exception leaves this module.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {

enum class WindowMode { kWindowed, kBorderless, kFullscreen };
enum class Scaling { kFit, kStretch };
enum class Antialiasing { kOff, kFxaa, kFxaaExtreme };
enum class Upscaler { kOff, kFsr, kDlss };
enum class FrameRate { k30, k60, k120, k144, kMonitor, kUnlimited };
enum class Vrr { kAuto, kOn, kOff };
// Frame generation: kFsr = FidelityFX proxy, kDlss = DLSS Frame Generation; turning on needs a restart.
enum class FrameGeneration { kOff, kFsr, kDlss };
// DLSS SR presets (NGX 0, 5, 6, 10, 11, 12, 13); A-D were removed by the 310 DLL, G-I and N-O are "do not use".
enum class DlssPreset { kDefault, kE, kF, kJ, kK, kL, kM };
// DLSS mode: kAuto derives it from internal_resolution, the others decide internal_resolution.
enum class DlssMode { kAuto, kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance };
enum class Aspect { k16x9, k21x9, k16x10 };
enum class ShadowQuality { kLow, kOriginal, kHigh };

// Engine options (Coalesced), all needing a restart. No texture LODBias: the cooker removed from the
// packages the mips excluded by the original LODBias (BULKDATA_Unused), lowering it shows never-written memory.
struct Engine {
  ShadowQuality shadow_quality = ShadowQuality::kOriginal;  // MaxShadowResolution 256 / 800 / 2048
  bool dynamic_shadows = true;                              // DynamicShadows
  bool operator==(const Engine&) const = default;
};

// Limits of hdr_peak_nits and hdr_paper_white_nits (0 = auto).
inline constexpr uint32_t kHdrPeakNitsMin = 100, kHdrPeakNitsMax = 10000;
inline constexpr uint32_t kHdrPaperWhiteNitsMin = 80, kHdrPaperWhiteNitsMax = 1000;
// Values of [video] ui_scale: size of the launcher and the F1 overlay as a percentage of the automatic scale
// (launcher::FitScale); 100 = as without the option.
inline constexpr std::array<uint32_t, 6> kUiScales{75, 90, 100, 110, 125, 150};

struct Video {
  WindowMode mode = WindowMode::kWindowed;
  uint32_t width = 0, height = 0;  // 0 = the mode's default
  Scaling scaling = Scaling::kFit;
  bool vsync = true;
  bool resizable = true;
  Antialiasing antialiasing = Antialiasing::kOff;
  bool sharpening = false;
  float sharpness = 0.0f;            // 0.0..1.0, used if sharpening
  uint32_t internal_resolution = 1;  // render resolution multiplier: 1, 2 or 3
  uint32_t anisotropic = 16;         // forced anisotropic filtering: 0 (the game's own), 2, 4, 8 or 16
  bool enhanced_shaders = true;         // replacement shaders by hash (gpu/shader_overrides.h)
  Upscaler upscaler = Upscaler::kOff;  // temporal upscaler (gpu/upscaler.h)
  Vrr vrr = Vrr::kAuto;                // fps limit below the monitor's refresh rate (VRR)
  FrameGeneration frame_generation = FrameGeneration::kOff;  // one generated frame between two real ones; turning on needs a restart
  bool hdr = false;                    // HDR10 output, needs a restart
  uint32_t hdr_peak_nits = 0;          // 0 = auto, otherwise 100..10000
  uint32_t hdr_paper_white_nits = 0;   // 0 = auto, otherwise 80..1000
  Aspect aspect = Aspect::k16x9;       // scene aspect ratio (21:9 / 16:10 patches); needs a restart
  DlssPreset dlss_preset = DlssPreset::kDefault;  // DLSS SR preset; needs a restart
  DlssMode dlss_mode = DlssMode::kAuto;           // DLSS SR mode; needs a restart
  uint32_t ui_scale = 100;             // size of the launcher and F1 overlay in % of the automatic scale (kUiScales); immediate
  bool operator==(const Video&) const = default;
};

// Codes of [game] language and of --language (kernel::xam::LanguageFromCode accepts them all).
inline constexpr std::array<std::string_view, 12> kLanguageCodes{"en", "ja", "de", "fr", "es", "it",
                                                                 "ko", "zh", "zh-cn", "pt", "pl", "ru"};

// Valid keys of [patches] (game patches, runtime/game_patches); settings does not depend on that module,
// a test checks that they match its table.
inline constexpr std::array<std::string_view, 4> kPatchKeys{"show_fps", "disable_motion_blur", "disable_lens_flares",
                                                            "disable_bloom"};

struct Settings {
  Video video;
  std::set<std::string> patches;  // keys of [patches] turned on (= true)
  std::string language;  // [game] language, empty = default (en)
  FrameRate frame_rate = FrameRate::k30;  // [game] frame_rate
  Engine engine;  // [engine]
  bool console = false;  // [debug] console: show the log console window (restart needed)
  std::vector<std::pair<std::string, std::string>> gpu_cvars;  // [gpu].cvars, in order
  bool operator==(const Settings&) const = default;
};

// Reads the file. Missing: nullopt and `out` unchanged. If invalid, the error is
// "<file>:<line>: <key>: <reason>" (line omitted if unknown) and `out` stays unchanged.
std::optional<std::string> LoadFile(const std::filesystem::path& path, Settings& out);

// TOML text equivalent to `s` (LoadFile of the text returns `s`): all the keys of [video]
// (width/height only if not 0), [game] always (frame_rate, and language if not empty), [engine] always (the three keys), [debug] only if console is true, [gpu] if there are cvars.
std::string Serialize(const Settings& s);

// Writes Serialize(s) to `path` via "<path>.tmp" and a rename (no truncated file
// if the process dies midway). Readable error, or nullopt; on error `path` stays as it was.
std::optional<std::string> SaveFile(const std::filesystem::path& path, const Settings& s);

// "WxH" with W in 320..7680 and H in 240..4320 (decimal digits, no sign).
bool ParseResolution(std::string_view text, uint32_t& w, uint32_t& h);

// "windowed" | "borderless" | "fullscreen" (lowercase only).
std::optional<WindowMode> ParseWindowMode(std::string_view text);

// "off" | "fxaa" | "fxaa_extreme" (lowercase only).
std::optional<Antialiasing> ParseAntialiasing(std::string_view text);

// Value of Xenia's anisotropic_override cvar: 0 -> -1 (the game's own), 2 -> 2, 4 -> 3, 8 -> 4, 16 -> 5; other -1.
int32_t AnisotropicOverride(uint32_t anisotropic);
// Name of the anisotropic_override cvar value for the log: 0 "off", 1 "1x", 2..5 "2x".."16x", other "game default".
std::string_view AnisotropicOverrideName(int32_t cvar);

// "off" | "fsr" | "dlss" (lowercase only).
std::optional<Upscaler> ParseUpscaler(std::string_view text);

// "off" | "fsr" | "dlss" (lowercase only); FrameGenerationName gives the same text.
std::optional<FrameGeneration> ParseFrameGeneration(std::string_view text);
std::string_view FrameGenerationName(FrameGeneration f);

// "default" | "e" | "f" | "j" | "k" | "l" | "m" (lowercase only); DlssPresetName gives the same text.
std::optional<DlssPreset> ParseDlssPreset(std::string_view text);
std::string_view DlssPresetName(DlssPreset p);

// "auto" | "dlaa" | "quality" | "balanced" | "performance" | "ultra_performance" (lowercase only);
// DlssModeName gives the same text.
std::optional<DlssMode> ParseDlssMode(std::string_view text);
std::string_view DlssModeName(DlssMode m);

// Window output: windowed width x height (0 = 1280x720), borderless the desktop, fullscreen
// width x height if not 0, otherwise the desktop.
std::pair<uint32_t, uint32_t> OutputSizeFor(const Video& v, uint32_t desktop_w, uint32_t desktop_h);
// Width of the image rectangle in the output: stretch the whole width, fit that of the scene's aspect ratio.
uint32_t ImageWidthFor(const Video& v, uint32_t out_w, uint32_t out_h);
uint32_t ImageHeightFor(const Video& v, uint32_t out_w, uint32_t out_h);
// Multiplier 1..3 (render 1280 * n) that gives the mode at that image width (the smallest);
// nullopt for kAuto and for unreachable modes (Xenia only scales by integers).
std::optional<uint32_t> InternalResolutionForDlssMode(DlssMode m, uint32_t image_w);
// Internal resolution to use: the mode's one with DLSS and a reachable mode, otherwise
// v.internal_resolution.
uint32_t EffectiveInternalResolution(const Video& v, uint32_t image_w);

// "30" | "60" | "120" | "144" | "monitor" | "unlimited" (lowercase only); FrameRateName gives the same text.
std::optional<FrameRate> ParseFrameRate(std::string_view text);
std::string_view FrameRateName(FrameRate rate);

// "auto" | "on" | "off" (lowercase only).
std::optional<Vrr> ParseVrr(std::string_view text);

// "16:9" | "21:9" | "16:10"; AspectName gives the same text.
std::optional<Aspect> ParseAspect(std::string_view text);
std::string_view AspectName(Aspect a);

// "low" | "original" | "high" (lowercase only); ShadowQualityName gives the same text.
std::optional<ShadowQuality> ParseShadowQuality(std::string_view text);
std::string_view ShadowQualityName(ShadowQuality q);

}  // namespace settings
