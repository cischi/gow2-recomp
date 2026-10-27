// hid driver for non-Xbox pads read through SDL3 (plat::SdlGamepads). It
// translates SDL snapshots into hid::Gamepad and assigns the pads to users 0-3
// not occupied by XInput, in connection order; the slot stays with the pad
// until it disconnects.
//
// All mutations of the slot table happen inside GetState and SetVibration,
// which hid::InputSystem calls with its own lock held: the driver has no locks
// of its own. GetState never blocks and never calls the OS (the
// plat::SdlGamepads snapshot is lock-free).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "hid/input_system.h"
#include "platform/sdl_gamepads.h"

namespace hid {

// Pure: SDL buttons -> XInput bits (GUIDE ignored), LEFTY/RIGHTY inverted
// (SDL has Y positive downwards), triggers 0..32767 -> 0..255.
Gamepad MapSdlGamepad(const plat::GamepadSnapshot&);

// Pad source: the real adapter sits on plat::SdlGamepads, the tests use a
// fake one.
class PadSource {
 public:
  virtual ~PadSource() = default;
  virtual size_t Snapshot(std::span<plat::GamepadSnapshot> out) const = 0;
  virtual void Rumble(uint32_t instance_id, uint16_t low, uint16_t high) = 0;
};

// Adapter: takes ownership of plat::SdlGamepads.
std::unique_ptr<PadSource> CreateSdlPadSource(std::unique_ptr<plat::SdlGamepads>);

// slot_taken_by_xinput(user): true if XInput has a pad on that user. It runs
// under the InputSystem lock on every GetState: it must read only cached
// state, without calling the OS. If it becomes true for the user of an SDL
// pad, the pad leaves the slot ("disconnected" line, vibration cleared with
// PadSource::Rumble(id, 0, 0)) and takes the first free user
// ("connected"); with no free users it stays without a user and is retried. The lines
// "[input] SDL pad connected|disconnected: <name> (<type>) -> user N" go
// to `log` (default: LogLine).
std::unique_ptr<InputDriver> CreateSdlDriver(std::unique_ptr<PadSource> source,
                                             std::function<bool(uint32_t user)> slot_taken_by_xinput,
                                             std::function<void(std::string_view)> log = {});

}  // namespace hid
