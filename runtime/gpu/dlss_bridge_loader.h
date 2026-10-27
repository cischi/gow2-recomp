// Loads gow2_dlss.dll (runtime/gpu/dlss_bridge/gow2_dlss_bridge.h) and checks its ABI; shared by the DLSS
// upscaler (SR) and DLSS frame generation.
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "gpu/dlss_bridge/gow2_dlss_bridge.h"

namespace gpu {

// The DLL stays loaded: the caller frees it (FreeLibrary) after destroying the contexts.
struct DlssBridge {
  HMODULE dll;
  const Gow2DlssApi* api;
};

// Accepts abi 1..GOW2_DLSS_BRIDGE_ABI; the FG functions are valid only with api->abi >= 3. nullopt: `why` says
// the reason (with the DLL path); the caller writes it after "DLSS unavailable: ".
std::optional<DlssBridge> LoadDlssBridge(const std::filesystem::path& dll_dir, std::string& why);

}  // namespace gpu
