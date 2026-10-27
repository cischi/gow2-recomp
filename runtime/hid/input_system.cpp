#include "kernel/registry.h"

#include "hid/input_system.h"

#include <array>
#include <chrono>
#include <mutex>
#include <thread>

#include "kernel/guest.h"
#include "kernel/stats.h"

namespace hid {
namespace {

constexpr uint32_t kMaxUsers = 4;

std::mutex g_mutex;
std::vector<std::unique_ptr<InputDriver>> g_drivers;
struct PacketState {
  bool valid = false;
  Gamepad last;
  uint32_t packet = 0;
};
std::array<PacketState, kMaxUsers> g_packets;

// Layout of X_INPUT_STATE / X_INPUT_CAPABILITIES / X_INPUT_VIBRATION (Xenia hid/input.h).
struct GuestGamepad {
  be<uint16_t> buttons;
  uint8_t left_trigger, right_trigger;
  be<int16_t> thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
struct GuestState {
  be<uint32_t> packet_number;
  GuestGamepad gamepad;
};
struct GuestCaps {
  uint8_t type, sub_type;
  be<uint16_t> flags;
  GuestGamepad gamepad;
  be<uint16_t> vibration_left, vibration_right;
};
struct GuestVibration {
  be<uint16_t> left, right;
};
static_assert(sizeof(GuestState) == 16 && sizeof(GuestCaps) == 20 && sizeof(GuestVibration) == 4);

// Effective user (xam_input.cc: 0xFF or ANY_USER -> 0).
uint32_t PinUser(uint32_t user, uint32_t flags) {
  return ((user & 0xFF) == 0xFF || (flags & kFlagAnyUser)) ? 0 : user;
}
bool NotGamepadQuery(uint32_t flags) { return (flags & 0xFF) && !(flags & kFlagGamepad); }

// First driver that answers; with g_mutex held.
std::optional<Gamepad> Poll(uint32_t user) {
  for (auto& d : g_drivers)
    if (auto g = d->GetState(user)) return g;
  return std::nullopt;
}

}  // namespace

void Init(std::vector<std::unique_ptr<InputDriver>> drivers) {
  std::vector<std::unique_ptr<InputDriver>> old;  // destroyed outside the lock
  std::lock_guard lock(g_mutex);
  old.swap(g_drivers);
  g_drivers = std::move(drivers);
  g_packets = {};
}

void Shutdown() { Init({}); }

uint32_t GetState(uint32_t user, uint32_t flags, uint32_t state_ptr) {
  kernel::stats::NoteInputGetState();
  if (NotGamepadQuery(flags)) return kErrorDeviceNotConnected;
  user = PinUser(user, flags);
  if (user >= kMaxUsers) return kErrorDeviceNotConnected;
  GuestState out;
  {
    std::lock_guard lock(g_mutex);
    const auto g = Poll(user);
    if (!g) return kErrorDeviceNotConnected;
    PacketState& p = g_packets[user];
    if (!p.valid || !(p.last == *g)) {
      p.valid = true;
      p.last = *g;
      ++p.packet;
    }
    out.packet_number = p.packet;
    out.gamepad = {g->buttons, g->left_trigger, g->right_trigger, g->thumb_lx,
                   g->thumb_ly, g->thumb_rx,    g->thumb_ry};
  }
  if (state_ptr) {
    if (auto* dst = kernel::GuestPtr<GuestState>(state_ptr)) *dst = out;
  }
  return kErrorSuccess;
}

uint32_t GetCapabilities(uint32_t user, uint32_t flags, uint32_t caps_ptr) {
  if (!caps_ptr) return kErrorBadArguments;
  if (NotGamepadQuery(flags)) return kErrorDeviceNotConnected;
  user = PinUser(user, flags);
  if (user >= kMaxUsers) return kErrorDeviceNotConnected;
  {
    std::lock_guard lock(g_mutex);
    if (!Poll(user)) return kErrorDeviceNotConnected;
  }
  GuestCaps caps{};
  caps.type = 1;
  caps.sub_type = 1;
  caps.flags = 0;
  caps.gamepad = {0xFFFF, 0xFF, 0xFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
  caps.vibration_left = 0xFFFF;
  caps.vibration_right = 0xFFFF;
  if (auto* dst = kernel::GuestPtr<GuestCaps>(caps_ptr)) *dst = caps;
  return kErrorSuccess;
}

bool StopAllVibration(std::chrono::milliseconds lock_timeout) {
  // try_lock at intervals instead of lock: whoever is exiting is never blocked by
  // a stalled thread holding the lock (std::timed_mutex would slow down GetState).
  std::unique_lock lock(g_mutex, std::try_to_lock);
  const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
  while (!lock.owns_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (lock.try_lock()) break;
  }
  for (auto& d : g_drivers)
    for (uint32_t user = 0; user < kMaxUsers; ++user) d->SetVibration(user, 0, 0);
  return true;
}

uint32_t SetState(uint32_t user, uint32_t /*unk*/, uint32_t vibration_ptr) {
  if (!vibration_ptr) return kErrorBadArguments;
  auto* v = kernel::GuestPtr<GuestVibration>(vibration_ptr);
  if (!v) return kErrorBadArguments;
  const uint16_t left = v->left, right = v->right;
  if ((user & 0xFF) == 0xFF) user = 0;
  if (user >= kMaxUsers) return kErrorDeviceNotConnected;
  std::lock_guard lock(g_mutex);
  for (auto& d : g_drivers)
    if (d->SetVibration(user, left, right)) return kErrorSuccess;
  return kErrorDeviceNotConnected;
}

}  // namespace hid
