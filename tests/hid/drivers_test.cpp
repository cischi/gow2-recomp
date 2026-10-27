// Input drivers (runtime/hid/drivers.*): script, keyboard, XInput.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "hid/drivers.h"
#include "hid/input_script.h"
#include "kernel/stats.h"

using namespace std::chrono_literals;

namespace {

uint64_t Counter(const std::string& name) {
  const std::string s = kernel::stats::Summary();
  const std::string key = "[summary] " + name + "=";
  const size_t pos = s.find(key);
  REQUIRE(pos != std::string::npos);
  return std::stoull(s.substr(pos + key.size()));
}

}  // namespace

// The "since start" instant is chosen by moving `start` into the past: no
// real waits, no dependency on the scheduler.
TEST_CASE("drivers: script, button pressed between at and at+hold, user 0 always connected", "[drivers]") {
  const std::vector<hid::ScriptEvent> events = {{100, 0x0010, 50}};
  const auto now = std::chrono::steady_clock::now();

  auto before = hid::CreateScriptDriver(events, now - 50ms);
  auto s50 = before->GetState(0);
  REQUIRE(s50);
  CHECK(s50->buttons == 0);
  CHECK(*s50 == hid::Gamepad{});
  CHECK_FALSE(before->GetState(1));
  CHECK_FALSE(before->GetState(3));

  const uint64_t injected = Counter("input_injected_events");
  auto during = hid::CreateScriptDriver(events, std::chrono::steady_clock::now() - 120ms);
  for (int i = 0; i < 10; ++i) {
    auto s = during->GetState(0);
    REQUIRE(s);
    CHECK(s->buttons == 0x0010);
  }
  CHECK(Counter("input_injected_events") - injected == 1);  // once per event

  auto after = hid::CreateScriptDriver(events, std::chrono::steady_clock::now() - 200ms);
  auto s200 = after->GetState(0);
  REQUIRE(s200);
  CHECK(s200->buttons == 0);
  CHECK(Counter("input_injected_events") - injected == 1);  // never seen pressed: does not count

  CHECK(during->SetVibration(0, 1, 2));
  CHECK_FALSE(during->SetVibration(1, 1, 2));
}

TEST_CASE("drivers: script, overlapping events combine", "[drivers]") {
  auto d = hid::CreateScriptDriver({{0, 0x1000, 1000}, {0, 0x0010, 1000}, {5000, 0x2000, 100}},
                                   std::chrono::steady_clock::now() - 10ms);
  auto s = d->GetState(0);
  REQUIRE(s);
  CHECK(s->buttons == 0x1010);
}

TEST_CASE("drivers: keyboard without a window, user 0 connected with empty state", "[drivers]") {
  auto k = hid::CreateKeyboardDriver(nullptr);
  REQUIRE(k);
  auto s = k->GetState(0);
  REQUIRE(s);
  CHECK(*s == hid::Gamepad{});
  CHECK_FALSE(k->GetState(1));
  CHECK_FALSE(k->GetState(4));
  CHECK(k->SetVibration(0, 100, 100));
  CHECK_FALSE(k->SetVibration(1, 100, 100));
}

TEST_CASE("drivers: XInput does not crash (nullptr if xinput1_4.dll is missing)", "[drivers]") {
  auto x = hid::CreateXInputDriver();
  if (!x) SUCCEED("xinput1_4.dll missing");
  else {
    for (uint32_t u = 0; u < 4; ++u) (void)x->GetState(u);
    CHECK_FALSE(x->GetState(4));
    CHECK_FALSE(x->SetVibration(4, 0, 0));
    x.reset();  // the polling thread stops
  }
}
