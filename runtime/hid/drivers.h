// gow2 input drivers (hid::InputDriver). The order in main is: script
// (only with --input-script), XInput, SDL (sdl_driver.h), keyboard; the keyboard is last, so
// virtual pad 0 shows up as connected even without an XInput pad.
//
// hid::GetState calls the drivers' GetState with the InputSystem lock held and
// the game calls XamInputGetState thousands of times per second: the GetState
// of these drivers never blocks and never calls the OS (atomics or cached state).
#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "hid/input_script.h"
#include "hid/input_system.h"

namespace plat {
struct Window;
}

namespace hid {

// User 0 always connected (the others are not); buttons pressed from start + at to
// start + at + hold (exclusive), OR of the active events, sticks and triggers at 0.
// kernel::stats::NoteInputInjectedEvent once per event, on the first
// read that sees it pressed (an event never read while pressed does not count).
// SetVibration: true for user 0 (ignored).
std::unique_ptr<InputDriver> CreateScriptDriver(std::vector<ScriptEvent>, std::chrono::steady_clock::time_point start);

// XInput 1.4 pads, users 0..3; nullptr if xinput1_4.dll is missing. A dedicated
// thread ("gow2 xinput") polls connected pads every 4 ms and disconnected
// ones every second (XInputGetState is slow without a pad); GetState
// reads the cached state (seqlock, no lock). SetVibration calls
// XInputSetState only for a connected pad.
std::unique_ptr<InputDriver> CreateXInputDriver();

// Keyboard of the game window, user 0 always connected; empty state
// without focus or with a null Window* (--gpu fake). Map: Enter = START,
// Esc = BACK, Space = A, W/S = ly +/-32767, A/D = lx -/+32767 (opposite keys
// pressed together: 0), arrows = d-pad. Reads the atomics of plat::IsKeyDown.
// SetVibration: true for user 0 (ignored).
std::unique_ptr<InputDriver> CreateKeyboardDriver(plat::Window*);

}  // namespace hid
