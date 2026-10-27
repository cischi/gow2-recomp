// See dlss_bridge_loader.h.
#include "gpu/dlss_bridge_loader.h"

namespace gpu {

std::optional<DlssBridge> LoadDlssBridge(const std::filesystem::path& dll_dir, std::string& why) {
  const std::filesystem::path path = dll_dir / L"gow2_dlss.dll";
  HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) {
    why = path.string() + " could not be loaded (error " + std::to_string(GetLastError()) + ")";
    return std::nullopt;
  }
  const auto get = reinterpret_cast<Gow2DlssGetApiFn>(GetProcAddress(dll, GOW2_DLSS_GET_API));
  const Gow2DlssApi* api = get ? get() : nullptr;
  if (!api || api->abi < 1 || api->abi > GOW2_DLSS_BRIDGE_ABI || !api->Create || !api->Evaluate || !api->Destroy ||
      (api->abi >= 2 && !api->CreateEx) ||
      (api->abi >= 3 &&
       (!api->FgQuery || !api->FgCreate || !api->FgEvaluate || !api->FgDescription || !api->FgDestroy))) {
    why = path.string() + " has an ABI different from " + std::to_string(GOW2_DLSS_BRIDGE_ABI) +
          (api ? " (" + std::to_string(api->abi) + ")" : "");
    FreeLibrary(dll);
    return std::nullopt;
  }
  return DlssBridge{dll, api};
}

}  // namespace gpu
