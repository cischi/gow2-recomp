// XInput 1.4 pad (platform.h). Model: src/xenia/hid/xinput/xinput_input_driver.cc
// of Xenia @ 95a5c3ee (BSD): xinput1_4.dll loaded at runtime, functions taken
// with GetProcAddress, no static import (the executable starts without it too).
#include <windows.h>
#include <xinput.h>

#include "platform.h"

namespace plat {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

struct XInputApi {
  GetStateFn get_state = nullptr;
  SetStateFn set_state = nullptr;
};

// Loaded once (thread-safe static initialization); the DLL stays
// loaded until exit.
const XInputApi& Api() {
  static const XInputApi api = [] {
    XInputApi a;
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    if (!module) return a;
    a.get_state = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
    a.set_state = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")));
    if (!a.get_state || !a.set_state) a = {};
    return a;
  }();
  return api;
}

}  // namespace

bool XInputAvailable() { return Api().get_state != nullptr; }

std::optional<PadState> XInputGetState(uint32_t user) {
  const auto& api = Api();
  if (!api.get_state || user >= XUSER_MAX_COUNT) return std::nullopt;
  XINPUT_STATE state{};
  if (api.get_state(user, &state) != ERROR_SUCCESS) return std::nullopt;
  const XINPUT_GAMEPAD& g = state.Gamepad;
  return PadState{g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
}

bool XInputSetVibration(uint32_t user, uint16_t left, uint16_t right) {
  const auto& api = Api();
  if (!api.set_state || user >= XUSER_MAX_COUNT) return false;
  XINPUT_VIBRATION vibration{left, right};
  return api.set_state(user, &vibration) == ERROR_SUCCESS;
}

}  // namespace plat
