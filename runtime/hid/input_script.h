// --input-script "<s>:<BUTTON>[+<ms>],...": scripted presses of virtual
// pad 0 (diagnostics and automated tests), served by hid::CreateScriptDriver
// (hid/drivers.h).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hid {

// Button bits (X_INPUT_GAMEPAD, identical to Windows' XINPUT_GAMEPAD).
inline constexpr uint16_t kButtonDpadUp = 0x0001, kButtonDpadDown = 0x0002, kButtonDpadLeft = 0x0004,
                          kButtonDpadRight = 0x0008, kButtonStart = 0x0010, kButtonBack = 0x0020,
                          kButtonLeftThumb = 0x0040, kButtonRightThumb = 0x0080, kButtonLeftShoulder = 0x0100,
                          kButtonRightShoulder = 0x0200, kButtonA = 0x1000, kButtonB = 0x2000, kButtonX = 0x4000,
                          kButtonY = 0x8000;

// Button `buttons` pressed from at_ms to at_ms + hold_ms (exclusive) since start.
struct ScriptEvent {
  uint32_t at_ms;
  uint16_t buttons;
  uint32_t hold_ms;
  int16_t lx = 0, ly = 0;  // LS_UP/LS_DOWN/LS_LEFT/LS_RIGHT (left stick fully deflected)
  uint8_t lt = 0, rt = 0;  // LT/RT fully pressed
};

// Events separated by ',' (none empty), each "<s>:<BUTTON>[+<ms>]":
//  - s: seconds since start, integer 0..86400 or with at most 3 decimals
//    (".5" and "1." are invalid), at most 86400;
//  - BUTTON (uppercase): DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT START BACK
//    LTHUMB RTHUMB LB RB A B X Y, or LS_UP LS_DOWN LS_LEFT LS_RIGHT (left
//    stick fully deflected) LT RT (trigger fully pressed);
//  - ms: press duration, integer 1..60000, default 100.
// nullopt if malformed (also the empty string). Events in the given order.
std::optional<std::vector<ScriptEvent>> ParseInputScript(std::string_view);

}  // namespace hid
