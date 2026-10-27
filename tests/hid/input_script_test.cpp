// --input-script (runtime/hid/input_script.*): grammar "<s>:<BUTTON>[+<ms>],...".
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "hid/input_script.h"

namespace {

bool Same(const std::vector<hid::ScriptEvent>& got, const std::vector<hid::ScriptEvent>& want) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < got.size(); ++i)
    if (got[i].at_ms != want[i].at_ms || got[i].buttons != want[i].buttons || got[i].hold_ms != want[i].hold_ms)
      return false;
  return true;
}

}  // namespace

TEST_CASE("input_script: valid events", "[input_script]") {
  auto a = hid::ParseInputScript("40:START+250");
  REQUIRE(a);
  CHECK(Same(*a, {{40000, 0x0010, 250}}));

  auto b = hid::ParseInputScript("1.5:A,2:B+10");
  REQUIRE(b);
  CHECK(Same(*b, {{1500, 0x1000, 100}, {2000, 0x2000, 10}}));

  auto c = hid::ParseInputScript("0:Y");
  REQUIRE(c);
  CHECK(Same(*c, {{0, 0x8000, 100}}));

  // all the buttons and the limits
  struct {
    const char* name;
    uint16_t bit;
  } const buttons[] = {{"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
                       {"START", 0x0010},   {"BACK", 0x0020},      {"LTHUMB", 0x0040},    {"RTHUMB", 0x0080},
                       {"LB", 0x0100},      {"RB", 0x0200},        {"A", 0x1000},         {"B", 0x2000},
                       {"X", 0x4000},       {"Y", 0x8000}};
  for (const auto& button : buttons) {
    INFO(button.name);
    auto e = hid::ParseInputScript(std::string("86400:") + button.name + "+60000");
    REQUIRE(e);
    CHECK(Same(*e, {{86400000, button.bit, 60000}}));
  }
  auto d = hid::ParseInputScript("0.125:LB+1");
  REQUIRE(d);
  CHECK(Same(*d, {{125, 0x0100, 1}}));
}

TEST_CASE("input_script: left stick and triggers", "[input_script]") {
  auto e = hid::ParseInputScript("1:LS_UP+300,2:LS_LEFT,3:LT+400,4:RT");
  REQUIRE(e);
  REQUIRE(e->size() == 4);
  CHECK(((*e)[0].ly == 32767 && (*e)[0].lx == 0 && (*e)[0].buttons == 0 && (*e)[0].hold_ms == 300));
  CHECK(((*e)[1].lx == -32767 && (*e)[1].ly == 0));
  CHECK(((*e)[2].lt == 255 && (*e)[2].rt == 0 && (*e)[2].hold_ms == 400));
  CHECK(((*e)[3].rt == 255 && (*e)[3].lt == 0));
}

TEST_CASE("input_script: invalid scripts", "[input_script]") {
  for (const char* bad : {"", "40", "40:", "40:start", "-1:A", "86401:A", "40:A+0", "40:A+60001", "40:A,,41:B",
                          "40:GUIDE", "1.2345:A", "40:A,", ",40:A", "1.:A", ".5:A", "40:A+", "40:A+1x", "86400.001:A",
                          " 40:A", "40:A+-1"}) {
    INFO(bad);
    CHECK_FALSE(hid::ParseInputScript(bad).has_value());
  }
}
