// NVIDIA DLSS runtime DLLs (nvngx_dlss.dll, nvngx_dlssg.dll): not redistributable in our release, so the launcher
// downloads them from NVIDIA's official GitHub repo (pinned tag, verified SHA-256) next to gow2.exe.
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "launcher/optiscaler.h"

namespace nvidia_dlls {

struct Dll {
  std::wstring_view url;
  std::string_view name, sha256;
};

inline constexpr std::array<Dll, 2> kDlls{{
    {L"https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll",
     "nvngx_dlss.dll", "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"},
    {L"https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlssg.dll",
     "nvngx_dlssg.dll", "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"},
}};

// All-or-nothing: every missing (or wrong-hash) DLL is downloaded to <name>.download and verified; only then are
// they renamed into place. On any failure the .download files are deleted and exe_dir is left as it was.
// nullopt = installed; otherwise the message for the panel. Cancel with optiscaler::CancelInstall().
std::optional<std::string> Install(const std::filesystem::path& exe_dir,
                                   const optiscaler::Fetch& fetch = optiscaler::HttpFetch,
                                   std::span<const Dll> dlls = kDlls);

}  // namespace nvidia_dlls
