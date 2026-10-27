// See sdl_driver.h.
#include "hid/sdl_driver.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>

#include "hid/input_script.h"
#include "log.h"

namespace hid {
namespace {

constexpr uint32_t kUsers = 4;
constexpr size_t kMaxPads = 8;

// SDL_GamepadButton index -> XInput bit (0 = ignored; 5 = GUIDE). The order is
// that of SDL_GamepadButton, documented in platform/sdl_gamepads.h
// (SOUTH=0 ... DPAD_RIGHT=14).
constexpr std::array<uint16_t, 15> kButtonMap = {
    kButtonA,     kButtonB,         kButtonX,         kButtonY,           kButtonBack,
    0,            kButtonStart,     kButtonLeftThumb, kButtonRightThumb,  kButtonLeftShoulder,
    kButtonRightShoulder, kButtonDpadUp, kButtonDpadDown, kButtonDpadLeft, kButtonDpadRight};

static_assert(kButtonMap.size() == 15, "SDL_GAMEPAD_BUTTON_SOUTH..DPAD_RIGHT are 15 buttons");

int16_t InvertAxis(int16_t v) { return v == INT16_MIN ? INT16_MAX : int16_t(-v); }
uint8_t Trigger(int16_t v) { return uint8_t(std::clamp(int(v), 0, 32767) * 255 / 32767); }

class LivePadSource final : public PadSource {
 public:
  explicit LivePadSource(std::unique_ptr<plat::SdlGamepads> pads) : pads_(std::move(pads)) {}
  size_t Snapshot(std::span<plat::GamepadSnapshot> out) const override { return pads_->Snapshot(out); }
  void Rumble(uint32_t id, uint16_t low, uint16_t high) override { pads_->Rumble(id, low, high); }

 private:
  std::unique_ptr<plat::SdlGamepads> pads_;
};

class SdlDriver final : public InputDriver {
 public:
  SdlDriver(std::unique_ptr<PadSource> source, std::function<bool(uint32_t)> taken,
            std::function<void(std::string_view)> log)
      : source_(std::move(source)), taken_(std::move(taken)), log_(log ? std::move(log) : LogLine) {}

  std::optional<Gamepad> GetState(uint32_t user) override {
    if (user >= kUsers) return std::nullopt;
    std::array<plat::GamepadSnapshot, kMaxPads> snap;
    const size_t n = Sync(snap);
    if (!slots_[user]) return std::nullopt;
    for (size_t i = 0; i < n; ++i)
      if (snap[i].instance_id == slots_[user]->instance_id) return MapSdlGamepad(snap[i]);
    return std::nullopt;
  }

  bool SetVibration(uint32_t user, uint16_t left, uint16_t right) override {
    if (user >= kUsers) return false;
    std::array<plat::GamepadSnapshot, kMaxPads> snap;
    Sync(snap);
    if (!slots_[user]) return false;
    source_->Rumble(slots_[user]->instance_id, left, right);
    return true;
  }

 private:
  struct Slot {
    uint32_t instance_id;
    std::string name, type;
  };

  void Log(const char* what, const Slot& s, uint32_t user) {
    log_(std::string("[input] SDL pad ") + what + ": " + s.name + " (" + s.type + ") -> user " +
         std::to_string(user));
  }

  // Aligns the slots with the snapshot; returns how many pads it contains.
  size_t Sync(std::array<plat::GamepadSnapshot, kMaxPads>& snap) {
    const size_t n = std::min(source_->Snapshot(snap), snap.size());
    auto present = [&](uint32_t id) {
      return std::any_of(snap.begin(), snap.begin() + n, [&](const auto& p) { return p.instance_id == id; });
    };
    for (uint32_t u = 0; u < kUsers; ++u) {
      // Disconnected, or shadowed by XInput (which the system queries first).
      if (slots_[u] && (!present(slots_[u]->instance_id) || (taken_ && taken_(u)))) {
        Log("disconnected", *slots_[u], u);
        // The pad stays powered (shadowed, or waiting for a user) and SDL
        // vibration does not expire on its own: it is cleared. On a
        // disconnected pad the id is unknown and the request does nothing.
        source_->Rumble(slots_[u]->instance_id, 0, 0);
        slots_[u].reset();
      }
    }
    for (size_t i = 0; i < n; ++i) {  // the snapshot is in connection order
      const auto& p = snap[i];
      const bool assigned = std::any_of(slots_.begin(), slots_.end(),
                                        [&](const auto& s) { return s && s->instance_id == p.instance_id; });
      if (assigned) continue;
      for (uint32_t u = 0; u < kUsers; ++u) {
        if (slots_[u] || (taken_ && taken_(u))) continue;
        slots_[u] = Slot{p.instance_id, std::string(p.name, strnlen(p.name, sizeof p.name)),
                         std::string(p.type, strnlen(p.type, sizeof p.type))};
        Log("connected", *slots_[u], u);
        break;
      }
    }
    return n;
  }

  std::unique_ptr<PadSource> source_;
  std::function<bool(uint32_t)> taken_;
  std::function<void(std::string_view)> log_;
  std::array<std::optional<Slot>, kUsers> slots_;
};

}  // namespace

Gamepad MapSdlGamepad(const plat::GamepadSnapshot& s) {
  Gamepad g;
  for (size_t i = 0; i < kButtonMap.size(); ++i)
    if (s.buttons >> i & 1) g.buttons |= kButtonMap[i];
  g.thumb_lx = s.axes[0];
  g.thumb_ly = InvertAxis(s.axes[1]);
  g.thumb_rx = s.axes[2];
  g.thumb_ry = InvertAxis(s.axes[3]);
  g.left_trigger = Trigger(s.axes[4]);
  g.right_trigger = Trigger(s.axes[5]);
  return g;
}

std::unique_ptr<PadSource> CreateSdlPadSource(std::unique_ptr<plat::SdlGamepads> pads) {
  return std::make_unique<LivePadSource>(std::move(pads));
}

std::unique_ptr<InputDriver> CreateSdlDriver(std::unique_ptr<PadSource> source,
                                             std::function<bool(uint32_t)> slot_taken_by_xinput,
                                             std::function<void(std::string_view)> log) {
  return std::make_unique<SdlDriver>(std::move(source), std::move(slot_taken_by_xinput), std::move(log));
}

}  // namespace hid
