// Launcher window (Win32 + Direct3D 11 + Dear ImGui) before the game starts:
// shows the settings of `config` and writes them with Play. Nothing stays alive
// on return (window, device, ImGui context).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hdr_math.h"
#include "launcher/launcher_ui.h"

namespace launcher {

enum class Result { kPlay, kQuit, kError };

struct LaunchOptions {
  // Tests: after max_frames frames (0 = never) performs auto_action as if the
  // button had been pressed (kNone = Quit). This way a failed save returns kError.
  uint32_t max_frames = 0;
  Action auto_action = Action::kNone;
};

// kPlay: file written, the game starts. kQuit: Quit or window closed, file untouched.
// kError: window or Direct3D 11 unavailable (reason in *error), file untouched.
Result Run(const std::filesystem::path& config, const LaunchOptions& options = {}, std::string* error = nullptr);

// Modes of the primary monitor (EnumDisplaySettingsW), with duplicates: for ResolutionChoices.
std::vector<Resolution> MonitorModes();
// Windows UI in Italian (GetUserDefaultUILanguage): menu language without [game] language.
bool WindowsUiIsItalian();
// Folder of gow2.exe (where the build copies DLLs and licenses).
std::filesystem::path ExeDir();
// The DLSS bridge gow2_dlss.dll and nvngx_dlss.dll are in dir: without them the launcher does not offer DLSS.
bool DlssBridgePresent(const std::filesystem::path& dir);
// Bridge present but nvngx_dlss.dll or nvngx_dlssg.dll missing: they can be downloaded (nvidia_dlls.h).
bool DlssDownloadAvailable(const std::filesystem::path& dir);
// nvngx_dlssnr.dll (OptiScaler's neural rendering) next to gow2.exe; the driver may still reject it. The project does not provide it.
bool DlssNrRuntimePresent(const std::filesystem::path& dir);
// The loader and DLLs of FSR 3 frame generation are in dir.
bool FrameGenerationDllsPresent(const std::filesystem::path& dir);
// The bridge gow2_dlss.dll and nvngx_dlssg.dll (DLSS frame generation) are in dir.
bool DlssFgDllsPresent(const std::filesystem::path& dir);
// DXGI output with the largest intersection with the window hwnd (HWND; -1 -> the first): ColorSpace G2084 P2020,
// MaxLuminance, DeviceName and, with HDR, DISPLAYCONFIG_SDR_WHITE_LEVEL in nits. Display{} on error.
// shortcut: output read at startup (no IsCurrent), window moved to another monitor or Windows HDR
// changed while the game is open: takes effect from the next launch.
hdr::Display QueryHdrDisplay(void* hwnd);

}  // namespace launcher
