// Fake input driver for tests: per-user state, recorded vibrations.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "hid/input_system.h"

namespace hid_test {

struct Vib {
  uint32_t user;
  uint16_t left, right;
};

class FakeInputDriver : public hid::InputDriver {
 public:
  std::array<std::optional<hid::Gamepad>, 4> state;
  std::vector<Vib>* vibrations = nullptr;  // owned by the test (the driver is owned by the system)

  std::optional<hid::Gamepad> GetState(uint32_t user) override { return user < 4 ? state[user] : std::nullopt; }
  bool SetVibration(uint32_t user, uint16_t left, uint16_t right) override {
    if (user >= 4 || !state[user]) return false;
    if (vibrations) vibrations->push_back({user, left, right});
    return true;
  }
};

}  // namespace hid_test
