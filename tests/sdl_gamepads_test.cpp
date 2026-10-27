// Non-Xbox pads with SDL3 (platform/sdl_gamepads.h): repeated start and stop,
// snapshots without pads, vibration on a nonexistent id, linked version.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include "platform/sdl_gamepads.h"

namespace {

std::unique_ptr<plat::SdlGamepads> StartOrFail() {
  std::string error;
  auto pads = plat::SdlGamepads::Start(&error);
  INFO("SDL error: " << error);
  REQUIRE(pads != nullptr);
  CHECK(error.empty());
  return pads;
}

// Without pads 0 entries; with a pad on the machine any number is accepted,
// but no entry may be an Xbox pad.
void CheckSnapshot(const plat::SdlGamepads& pads) {
  std::array<plat::GamepadSnapshot, 8> out{};
  const size_t n = pads.Snapshot(out);
  CHECK(n <= out.size());
  for (size_t i = 0; i < n; ++i) {
    const std::string_view type(out[i].type);
    INFO("pad " << out[i].name << " type " << type);
    CHECK(type.find("Xbox") == std::string_view::npos);
    CHECK(out[i].connect_order > 0);
  }
}

}  // namespace

TEST_CASE("sdl_gamepads: Start succeeds and Snapshot sees no Xbox pads", "[sdl_gamepads]") {
  auto pads = StartOrFail();
  CheckSnapshot(*pads);
  // empty span: no write, 0
  CHECK(pads->Snapshot({}) == 0);
}

TEST_CASE("sdl_gamepads: destruction and a new Start twice in a row", "[sdl_gamepads]") {
  for (int round = 0; round < 2; ++round) {
    auto pads = StartOrFail();
    CheckSnapshot(*pads);
    pads.reset();
  }
}

TEST_CASE("sdl_gamepads: a second Start with the first active fails with an error", "[sdl_gamepads]") {
  auto pads = StartOrFail();
  std::string error;
  CHECK(plat::SdlGamepads::Start(&error) == nullptr);
  CHECK(!error.empty());
  CheckSnapshot(*pads);
}

TEST_CASE("sdl_gamepads: Rumble on a nonexistent id does nothing", "[sdl_gamepads]") {
  auto pads = StartOrFail();
  pads->Rumble(0xFFFFFFF0u, 0xFFFF, 0xFFFF);
  pads->Rumble(0, 0, 0);
  CheckSnapshot(*pads);
}

TEST_CASE("sdl_gamepads: StopAllRumble waits for the SDL thread within the timeout", "[sdl_gamepads]") {
  // Without an active instance: nothing to stop.
  CHECK(plat::SdlGamepads::StopAllRumble(std::chrono::milliseconds(20)));
  auto pads = StartOrFail();
  pads->Rumble(0xFFFFFFF0u, 0xFFFF, 0xFFFF);
  // The SDL loop runs every 4 ms: 500 ms is enough even on a loaded machine.
  CHECK(plat::SdlGamepads::StopAllRumble(std::chrono::milliseconds(500)));
  CHECK(plat::SdlGamepads::StopAllRumble(std::chrono::milliseconds(500)));
  CheckSnapshot(*pads);
  pads.reset();
  CHECK(plat::SdlGamepads::StopAllRumble(std::chrono::milliseconds(20)));
}

TEST_CASE("sdl_gamepads: the linked version is 3.4.18", "[sdl_gamepads]") {
  CHECK(plat::SdlLinkedVersion() == 3004018);
}
