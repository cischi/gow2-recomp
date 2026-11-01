// Launcher settings panel: ImGui calls only (between the caller's NewFrame and
// Render), full viewport (launcher) or in a centered panel
// (F1 overlay in game, gpu/overlay.h).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "hdr_math.h"
#include "launcher/optiscaler.h"
#include "launcher/launcher_model.h"
#include "settings.h"

namespace launcher {

// Notes under the error; the text is in the frame's menu language (Text).
enum class Notice { kNone, kInvalidFile, kCommentsLost, kRestart };

// Menu language: UiLanguageFor(s.language, windows_italian) every frame, so it changes immediately
// when the user picks another Language.
struct LauncherState {
  settings::Settings s;
  std::vector<Resolution> resolutions;  // ResolutionChoices
  std::string error;        // in red at the top: LoadFile error or, with save_failed, the save error
  bool save_failed = false;  // error preceded by "Save failed: "
  Notice notice = Notice::kNone;
  std::vector<RestartField> restart;  // kRestart: fields that take effect from the next launch
  bool windows_italian = false;       // WindowsUiIsItalian(), for the default language
  bool dlss_available = false;        // DlssBridgePresent(ExeDir()): DLSS selectable
  bool fg_available = false;          // FrameGenerationDllsPresent(ExeDir()): FSR 3 entry selectable
  bool dlss_fg_available = false;     // DlssFgDllsPresent(ExeDir()): NVIDIA DLSS entry selectable
  optiscaler::State optiscaler = optiscaler::State::kMissing;  // Detect(ExeDir()): kInstalled blocks frame generation
  bool optiscaler_loaded = false;      // overlay: Loaded(ExeDir()); launcher: always false (no menu to open)
  bool optiscaler_installing = false;  // Install running on another thread: button off
  bool optiscaler_restart = false;     // installed in this process: takes effect after a restart
  std::string optiscaler_message;      // outcome of the last install (error or manual restart needed)
  bool dlss_download_available = false;  // DlssDownloadAvailable(ExeDir()): shows the download button
  bool dlss_downloading = false;         // download running on another thread: button off
  std::string dlss_download_message;     // outcome of the last download (error or success note)
  bool dlssnr_runtime = false;         // DlssNrRuntimePresent(ExeDir()): without it, a note under the button
  uint32_t desktop_w = 0, desktop_h = 0;  // plat::PrimaryMonitorSize(): borderless output for the DLSS mode
  hdr::Display hdr_display;           // QueryHdrDisplay(hwnd), once at opening
  bool focus_play = true;  // on the first frame focus goes to Play/Apply (Enter or the pad's A start it)
  int scroll_top_frames = 0;  // frames during which the panel is scrolled back to the top (at opening)
  bool data_ready = true;       // launcher: ExtractionComplete && CheckXex == kOk; false disables Play
  bool extracting = false;      // extraction running on another thread
  float extract_progress = 0;   // 0..1
  std::string data_message;     // last ISO check / extraction outcome (already in the UI language)
  std::set<std::string> unavailable;  // UnavailableOptions(): rows disabled, the file's value untouched
  bool overlay = false;    // Apply/Close buttons instead of Play/Quit
  float panel_w = 0, panel_h = 0;  // pixels; 0 = the whole viewport, otherwise a centered panel
};

enum class Action { kNone, kPlay, kQuit, kDefaults, kInstallOptiScaler, kOpenOptiScaler, kChooseIso,
              kCancelExtract, kDownloadDlss };

// Draws the panel and returns the button pressed in this frame.
Action DrawSettings(LauncherState& st);

}  // namespace launcher
