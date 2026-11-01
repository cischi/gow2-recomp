// Menu texts of the launcher and F1 overlay in English and Italian (UTF-8).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace launcher {

enum class UiLanguage { kEnglish, kItalian };
// Menu language: "it" -> Italian, any other code -> English; "" (game default) -> Italian
// if the Windows UI is Italian, otherwise English.
UiLanguage UiLanguageFor(std::string_view game_language, bool windows_italian);

// Only const char* (the test iterates over all of them).
struct UiText {
  const char *display, *mode, *windowed, *borderless, *fullscreen, *resolution, *res_default, *res_monitor,
      *resizable, *scaling, *letterbox, *stretch, *vsync;
  const char *image, *upscaler, *none, *dlss_unavailable, *antialiasing, *fxaa_extreme, *upscaler_replaces_fxaa,
      *sharpness, *sharpness_fsr, *dlss_no_sharpness, *internal_resolution, *enhanced_shaders,
      *anisotropic, *anisotropic_default, *anisotropic_tip;
  const char *game, *language;
  const char *defaults, *play, *quit, *apply, *close;
  const char *notice_invalid_file, *notice_comments_lost, *restart_prefix, *save_failed;
  const char *field_upscaler, *field_internal_resolution, *field_enhanced_shaders, *field_language,
      *field_frame_rate, *field_shadow_quality, *field_dynamic_shadows;
  // Frame rate ("Game" section) and VRR ("Display" section): labels, values and tooltips.
  const char *frame_rate, *frame_rate_original, *frame_rate_monitor, *frame_rate_unlimited, *frame_rate_tip, *vrr,
      *vrr_auto, *vrr_on, *vrr_off, *vrr_tip;
  // Game patches ("Game" section): label and tooltip (with the author) for each.
  const char *patch_show_fps, *patch_show_fps_tip, *patch_no_motion_blur, *patch_no_motion_blur_tip,
      *patch_no_lens_flares, *patch_no_lens_flares_tip, *patch_no_bloom, *patch_no_bloom_tip;
  // Engine configuration ("Image" section): labels, values and tooltips.
  const char *shadow_quality, *shadow_low, *shadow_original, *shadow_high, *shadow_quality_tip, *dynamic_shadows,
      *dynamic_shadows_tip;
  // Frame generation ("Image" section).
  const char *frame_generation, *frame_generation_tip, *frame_generation_unavailable, *field_frame_generation,
      *frame_generation_off, *frame_generation_dlss_tip, *frame_generation_dlss_unavailable, *frame_generation_optiscaler;
  // Aspect ratio ("Display" section).
  const char *aspect, *aspect_tip, *field_aspect;
  // HDR10 output ("Display" section); hdr_auto is a format with %u (nits).
  const char *hdr, *hdr_tip, *hdr_unavailable, *hdr_peak, *hdr_peak_tip, *hdr_paper_white, *hdr_paper_white_tip,
      *hdr_auto, *field_hdr;
  // Interface scale (button row), takes effect immediately.
  const char *ui_scale, *ui_scale_tip;
  // DLSS preset ("Image" section); K, M and L are NVIDIA names, not translated.
  const char *dlss_preset, *dlss_preset_tip, *field_dlss_preset, *dlss_preset_default;
  // DLSS mode ("Image" section): row, entries (DLAA is not translated), unreachable entry (format with the
  // image width and height), tooltip of the internal resolution decided by the mode; tooltips of the
  // preset dropdown entries (E and F share the text).
  const char *dlss_mode, *dlss_mode_tip, *field_dlss_mode, *dlss_mode_auto, *dlss_mode_quality, *dlss_mode_balanced,
      *dlss_mode_performance, *dlss_mode_ultra_performance, *dlss_mode_unreachable, *internal_resolution_from_mode,
      *dlss_preset_tip_ef, *dlss_preset_tip_j, *dlss_preset_tip_k, *dlss_preset_tip_l, *dlss_preset_tip_m;
  // OptiScaler ("Neural rendering" row of the "Image" section): optiscaler_replace and optiscaler_tip are formats
  // with %s (optiscaler::kVersion); optiscaler_replace_tip is appended to the replacement hint.
  const char *neural_rendering, *optiscaler_install, *optiscaler_replace, *optiscaler_tip, *optiscaler_replace_tip,
      *optiscaler_open, *optiscaler_installed, *optiscaler_restart, *optiscaler_installing, *optiscaler_foreign,
      *optiscaler_no_dlssnr, *optiscaler_needs_dlss, *optiscaler_restart_manual;
  // Game data (launcher only): section, state, ISO picking and extraction; iso_* are the IsoCheck messages.
  const char *game_data, *data_ready, *data_missing, *choose_iso, *extracting, *cancel_extract, *iso_not_an_iso,
      *iso_no_xex, *iso_wrong_edition, *iso_no_space, *extract_failed, *extract_cancelled, *extract_wrong_edition,
      *overwrite_data;  // Yes/No confirmation before extracting over game data that is already ready
  // Log console checkbox (section "Game"); needs a restart.
  const char *console, *console_tip, *field_console;
  // NVIDIA DLSS runtime download (section "Image").
  const char *dlss_download, *dlss_downloading, *dlss_download_note, *dlss_download_done;
  // Tooltip of the rows disabled by UnavailableOptions; HDR has its own reason.
  const char *not_available, *hdr_not_in_game;
};
// {game}, {edition} and {exe} in the texts are filled from game_profile::Get() (name, edition, id + ".exe").
const UiText& Text(UiLanguage);

// Fields that take effect only from the next launch (RestartNeeded), in this order.
enum class RestartField { kUpscaler, kInternalResolution, kEnhancedShaders, kLanguage, kFrameRate,
                              kShadowQuality, kDynamicShadows,
                              kFrameGeneration, kAspect, kHdr, kDlssPreset, kDlssMode, kConsole };
// Field names separated by ", ".
std::string RestartText(const std::vector<RestartField>& fields, UiLanguage);

}  // namespace launcher
