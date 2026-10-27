// Refresh rate of the window's monitor and VRR state (platform.h, DisplaySync).
// NVAPI: nvapi64.dll loaded at runtime with LoadLibraryExW from System32 only, never unloaded, no static imports;
// without it (non-NVIDIA GPU) the result is "unavailable" with the reason in `note`.
#include <windows.h>

#include <chrono>
#include <mutex>
#include <string>

#include "nvapi_vrr.h"
#include "platform.h"

namespace plat {
namespace {

static_assert(sizeof(NV_GET_VRR_INFO_V1) == 24, "layout of NV_GET_VRR_INFO_V1");

using QueryFn = void* (*)(NvU32);
using InitFn = int (*)();
using IdFn = int (*)(const char*, NvU32*);
using VrrFn = int (*)(NvU32, NV_GET_VRR_INFO_V1*);

struct Nvapi {
  IdFn display_id = nullptr;
  VrrFn vrr = nullptr;
  std::string error;  // non-empty = NVAPI not usable
};

// Once per process (thread-safe static initialization).
const Nvapi& GetNvapi() {
  static const Nvapi api = [] {
    Nvapi a;
    // From System32 only: a same-named DLL next to gow2.exe or in the current folder is not loaded.
    HMODULE dll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return a.error = "nvapi64.dll missing", a;
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(dll, "nvapi_QueryInterface"));
    if (!query) return a.error = "nvapi_QueryInterface missing", a;
    auto init = reinterpret_cast<InitFn>(query(kNvAPI_Initialize_Id));
    a.display_id = reinterpret_cast<IdFn>(query(kNvAPI_DISP_GetDisplayIdByDisplayName_Id));
    a.vrr = reinterpret_cast<VrrFn>(query(kNvAPI_Disp_GetVRRInfo_Id));
    if (!init || !a.display_id || !a.vrr) return a.error = "NVAPI functions missing", a;
    if (const int st = init(); st != NVAPI_OK) a.error = "NvAPI_Initialize st=" + std::to_string(st);
    return a;
  }();
  return api;
}

void QueryVrr(const wchar_t* gdi_name, DisplaySync& out) {
  const Nvapi& api = GetNvapi();
  if (!api.error.empty()) {
    out.note = api.error;
    return;
  }
  // The GDI name (\\.\DISPLAY1) is ASCII.
  std::string name;
  for (const wchar_t* c = gdi_name; *c; ++c) name += static_cast<char>(*c);
  NvU32 id = 0;
  if (const int st = api.display_id(name.c_str(), &id); st != NVAPI_OK) {
    out.note = "NvAPI_DISP_GetDisplayIdByDisplayName st=" + std::to_string(st);
    return;
  }
  NV_GET_VRR_INFO_V1 info{};
  info.version = NV_GET_VRR_INFO_VER1;
  if (const int st = api.vrr(id, &info); st != NVAPI_OK) {
    out.note = "NvAPI_Disp_GetVRRInfo st=" + std::to_string(st);
    return;
  }
  out.vrr = info.bIsVRREnabled != 0;
}

}  // namespace

DisplaySync QueryDisplaySync(Window* w, bool nvapi) {
  DisplaySync out;
  HWND hwnd = static_cast<HWND>(NativeHandle(w));
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    if (nvapi) out.note = "GetMonitorInfoW failed";
    return out;
  }
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    out.refresh_hz = dm.dmDisplayFrequency;
  if (nvapi) {
    const auto t0 = std::chrono::steady_clock::now();
    // NVAPI does not claim to be thread-safe: one call at a time.
    static std::mutex nvapi_mutex;
    std::lock_guard lock(nvapi_mutex);
    QueryVrr(mi.szDevice, out);
    out.nvapi_us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  return out;
}

}  // namespace plat
