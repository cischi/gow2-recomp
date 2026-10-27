// Input system: ordered drivers (the first that answers wins) and the three
// XAM operations with Xenia's semantics (https://github.com/xenia-project/xenia
// @ 95a5c3ee, BSD): src/xenia/kernel/xam/xam_input.cc and src/xenia/hid/input_system.cc.
// Standard library only: the real drivers live in runtime/platform.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hid {

struct Gamepad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
  bool operator==(const Gamepad&) const = default;
};

inline constexpr uint32_t kErrorSuccess = 0, kErrorDeviceNotConnected = 0x48F, kErrorBadArguments = 0xA0;
inline constexpr uint32_t kFlagGamepad = 1, kFlagAnyUser = 1u << 30;

class InputDriver {
 public:
  virtual ~InputDriver() = default;
  virtual std::optional<Gamepad> GetState(uint32_t user) = 0;                   // nullopt = not connected
  virtual bool SetVibration(uint32_t user, uint16_t left, uint16_t right) = 0;  // false = not connected
};

// Replaces the drivers (even with an empty list: no device) and resets
// the packet numbers.
void Init(std::vector<std::unique_ptr<InputDriver>> drivers_in_priority_order);
void Shutdown();

// Process exit (which goes through std::_Exit, without destructors):
// SetVibration(user, 0, 0) on every driver for users 0-3, with the
// InputSystem lock taken within `lock_timeout` at most (otherwise false,
// without touching the drivers). Not for crash handlers: it allocates and takes locks.
bool StopAllVibration(std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(20));

// Semantics of xam_input.cc: (flags & 0xFF) without kFlagGamepad -> not connected;
// user (u & 0xFF) == 0xFF or flags & kFlagAnyUser -> 0 (also for GetState,
// unlike Xenia which passes the original index).
uint32_t GetState(uint32_t user, uint32_t flags, uint32_t state_ptr);        // X_INPUT_STATE, 16 bytes BE; null ptr: result only
uint32_t GetCapabilities(uint32_t user, uint32_t flags, uint32_t caps_ptr);  // X_INPUT_CAPABILITIES, 20 bytes
uint32_t SetState(uint32_t user, uint32_t unk, uint32_t vibration_ptr);      // X_INPUT_VIBRATION, 4 bytes

}  // namespace hid
