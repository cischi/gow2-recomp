// Non-Xbox pads (DualSense, DualShock 4, Switch Pro...) read through SDL3
// (thirdparty/SDL, static, SDL_INIT_GAMEPAD only). Like the rest of
// runtime/platform it is the only place that talks to SDL; the hid driver
// translates the snapshots into Xbox pad state.
//
// A dedicated thread ("gow2 sdl") calls SDL_Init, pumps the connection
// events, every 4 ms copies the pad state into a lock-free cache
// (seqlock) and runs the vibration requests; in the destructor it clears
// the vibration, closes the pads and calls SDL_Quit on the same thread. Pads
// that SDL recognizes as Xbox 360 or Xbox One are excluded (they stay with XInput).
// SDL_HINT_TIMER_RESOLUTION is "0": SDL does not touch the process timer
// granularity (timeBeginPeriod).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace plat {

struct GamepadSnapshot {
  uint32_t instance_id;    // SDL_JoystickID, unique while the pad stays connected
  uint64_t connect_order;  // grows with every connection (1 = first)
  uint32_t buttons;        // bit i = SDL_GamepadButton i (SOUTH=0 ... DPAD_RIGHT=14)
  // LEFTX, LEFTY, RIGHTX, RIGHTY (-32768..32767, Y positive downwards),
  // LEFT_TRIGGER, RIGHT_TRIGGER (0..32767): SDL's order and values.
  int16_t axes[6];
  char name[64];  // pad name according to SDL (truncated, 0-terminated)
  char type[24];  // short SDL type name: "PS5", "PS4", "Switch Pro"...
};

class SdlGamepads {
 public:
  // Starts the thread and waits for SDL_Init(SDL_INIT_GAMEPAD). nullptr (with
  // SDL's message in *error, if not null) on failure.
  static std::unique_ptr<SdlGamepads> Start(std::string* error);
  // Stops the thread (SDL_Quit on the thread). One instance at a time.
  ~SdlGamepads();
  SdlGamepads(const SdlGamepads&) = delete;
  SdlGamepads& operator=(const SdlGamepads&) = delete;

  // Copies the connected non-Xbox pads into `out` (at most out.size()), in
  // connection order; returns how many. Lock-free, calls neither SDL nor the OS.
  size_t Snapshot(std::span<GamepadSnapshot> out) const;
  // Queues a vibration (0..65535 per motor, until the next command)
  // for the pad `instance_id`; executed by the SDL thread. Unknown id: nothing.
  void Rumble(uint32_t instance_id, uint16_t low, uint16_t high);
  // Exit: clears the vibration of all pads opened by the active instance
  // (also those without a user) and waits for the SDL thread to have executed it,
  // for no longer than `timeout`. true if executed (or no active instance),
  // false on timeout. From any thread; must not be called from a crash
  // handler (it takes mutexes and waits).
  static bool StopAllRumble(std::chrono::milliseconds timeout);

  struct Impl;

 private:
  explicit SdlGamepads(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Linked SDL version (SDL_GetVersion: major*1000000 + minor*1000 + micro).
int SdlLinkedVersion();

}  // namespace plat
