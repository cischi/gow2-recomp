// OptiScaler (DLSSNR-F5 fork) next to gow2.exe: dxgi.dll detection, hash verification,
// installation of the pinned version. No dependency on
// ImGui: used by main.cpp, the launcher and the F1 overlay.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "settings.h"

namespace optiscaler {

// Pinned version: updating it = changing these constants, testing in game, updating the README.
inline constexpr std::string_view kVersion = "v0.1.34";
inline constexpr std::wstring_view kZipUrl =
    L"https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases/download/v0.1.34/"
    L"OptiScaler-DLSSNR-F5-v0.1.34-with-dlss-fg.zip";
inline constexpr std::string_view kZipSha256 = "f1f0d8777fb337f8a89451b652fdc09ecd6c8b76ab96b20f1e4452eb5eefc11a";
// OptiScaler.dll inside the zip (installed as dxgi.dll).
inline constexpr std::string_view kDllSha256 = "fd35bc693ba4133fa10e9746ce84486d617bb2ac5ae76e096fbab77b28c8baa2";

// kInstalled: the pinned version; kOther: another OptiScaler; kForeign: a dxgi.dll that is not OptiScaler.
enum class State { kMissing, kOther, kInstalled, kForeign };
State Detect(const std::filesystem::path& exe_dir, std::string_view dll_sha256 = kDllSha256);
// With OptiScaler installed, frame generation is chosen from its menu: ours stays off (gow2.toml is not changed).
settings::FrameGeneration EffectiveFrameGeneration(settings::FrameGeneration wanted, State s);
// dxgi.dll loaded in this process and taken from exe_dir.
bool Loaded(const std::filesystem::path& exe_dir);
// Writes the default settings (table in optiscaler.cpp) into their sections; the rest stays identical.
// Empty if a key is missing from its section.
std::string PatchIni(std::string_view ini);

// Downloads url to dest: nullopt = succeeded, otherwise the reason.
using Fetch = std::function<std::optional<std::string>(std::wstring_view url, const std::filesystem::path& dest)>;
// Synchronous WinHTTP, follows redirects.
std::optional<std::string> HttpFetch(std::wstring_view url, const std::filesystem::path& dest);

// The string_views must stay valid for the whole Install call.
struct InstallConfig {
  std::wstring_view url = kZipUrl;
  std::string_view zip_sha256 = kZipSha256;
  std::string_view dll_sha256 = kDllSha256;
  Fetch fetch = HttpFetch;
};
// nullopt = installed; otherwise a message for the panel. Does not write to the log: the [optiscaler] line
// is written by the caller with the same text. On error exe_dir stays as it was (if a rollback fails the
// message says where the previous files are). Replaced files are renamed into exe_dir\.gow2_optiscaler_old
// (a loaded dxgi.dll cannot be deleted but can be renamed); files in OptiScaler\ that the zip lacks are kept.
std::optional<std::string> Install(const std::filesystem::path& exe_dir, const InstallConfig& cfg = {});

// Closing with an installation in progress (any thread). CancelInstall: the download (HttpFetch) stops and
// Install, if it has not started copying yet, returns "installation cancelled" without changing anything;
// a copy that has already started is not cancelled (it finishes or rolls back). Install clears the request at the start:
// it applies to the installation in progress, not to later ones. WaitInstallCopy waits for the copy in progress to finish
// (immediately if there is none). Before _Exit: CancelInstall() then WaitInstallCopy().
void CancelInstall();
void WaitInstallCopy();
// Clears a pending CancelInstall request: every download/install entry point (Install, nvidia_dlls::Install) calls
// it at the start, otherwise a cancel from an earlier session makes every later HttpFetch fail.
void ResetCancel();
bool CancelRequested();

// Deletes the backup copies of successful installations (best effort: DLLs still mapped stay for the
// next call). Install calls it at the start and at the end; also useful at launcher/game startup.
void SweepOld(const std::filesystem::path& exe_dir);

// Opens OptiScaler's menu: WM_KEYDOWN + WM_KEYUP of VK_INSERT posted to hwnd.
void OpenMenu(void* hwnd);

}  // namespace optiscaler
