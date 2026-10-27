// Launcher logic without ImGui or Win32 (testable): combo choices and
// settings to save.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdr_math.h"
#include "launcher/launcher_text.h"
#include "platform/platform.h"
#include "settings.h"

namespace launcher {

using Resolution = std::pair<uint32_t, uint32_t>;

// Monitor modes without duplicates and below 640x480, plus 1280x720 and (if not 0) the
// file's current resolution; sorted by area and then width, largest first.
std::vector<Resolution> ResolutionChoices(std::vector<Resolution> modes, uint32_t cur_w, uint32_t cur_h);

struct LanguageChoice {
  std::string_view code, label;  // label in UTF-8
};
// One entry for each settings::kLanguageCodes, in the same order.
std::span<const LanguageChoice> Languages();
// Label of a code ("" = default en); unknown code: the code itself.
std::string_view LanguageLabel(std::string_view code);

// Client area of the launcher in logical points (at 96 DPI).
inline constexpr int kWindowWidth = 640, kWindowHeight = 1160;

// Window scale: the DPI one, reduced as much as needed so that the client area
// (kWindowHeight x scale) plus border and title (frame_h px) fit in work_h px; never below 0.5.
float FitScale(float dpi_scale, int work_h, int frame_h);
// Panel scale: FitScale times ui_scale / 100 (settings::kUiScales; at 100% exactly FitScale).
float PanelScale(float dpi_scale, int work_h, int frame_h, uint32_t ui_scale);
// Panel height in px at scale `scale`: kWindowHeight x scale; above 100% at most `avail` (the
// content scrolls). At 100% and below no limit, as before.
float PanelHeight(float scale, uint32_t ui_scale, int avail);

// "<n>x (<W>x<H>)" with the aspect ratio's scene for n (1280x720, 1280x550, 1280x800); n outside 1..3 is clamped.
std::string InternalResolutionLabel(settings::Aspect aspect, uint32_t n);

// DLSS mode dropdown entry: the name, and with n "<name> - <render> -> <image>" (render 1280 x n for
// the aspect ratio's scene, image image_w x image_h: settings::ImageWidthFor / ImageHeightFor).
std::string DlssModeLabel(settings::DlssMode m, std::optional<uint32_t> n, settings::Aspect aspect, uint32_t image_w,
                          uint32_t image_h, UiLanguage l);

// Levels shown by "Auto": the screen's peak; paper white that ChooseLevels will use with the chosen peak (never above).
hdr::Levels AutoHdrLevels(uint32_t peak_setting, const hdr::Display& display);

// Fields that take effect only from the next launch (overlay) that differ between running and wanted,
// in RestartField order; empty if none.
std::vector<RestartField> RestartNeeded(const settings::Settings& running, const settings::Settings& wanted);

// Game window for the video settings: plat options (windowed: width x height, 0 = 1280x720)
// and exclusive fullscreen with its video mode (0 = the monitor's current mode).
struct WindowSetup {
  plat::WindowOptions window;
  uint32_t mode_width = 0, mode_height = 0;
  bool exclusive = false;
};
WindowSetup WindowSetupFor(const settings::Video& v);

}  // namespace launcher
