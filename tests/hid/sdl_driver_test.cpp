// hid driver for SDL pads (runtime/hid/sdl_driver.*) with a fake PadSource.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "hid/sdl_driver.h"

namespace {

plat::GamepadSnapshot Pad(uint32_t id, uint64_t order, const char* name = "DualSense", const char* type = "PS5") {
  plat::GamepadSnapshot s{};
  s.instance_id = id;
  s.connect_order = order;
  std::strncpy(s.name, name, sizeof s.name - 1);
  std::strncpy(s.type, type, sizeof s.type - 1);
  return s;
}

struct Rumble {
  uint32_t id;
  uint16_t low, high;
};

struct FakeState {
  std::vector<plat::GamepadSnapshot> pads;
  std::vector<Rumble> rumbles;
};

class FakeSource final : public hid::PadSource {
 public:
  explicit FakeSource(FakeState* st) : st_(st) {}
  size_t Snapshot(std::span<plat::GamepadSnapshot> out) const override {
    const size_t n = std::min(out.size(), st_->pads.size());
    for (size_t i = 0; i < n; ++i) out[i] = st_->pads[i];
    return n;
  }
  void Rumble(uint32_t id, uint16_t low, uint16_t high) override { st_->rumbles.push_back({id, low, high}); }

 private:
  FakeState* st_;
};

}  // namespace

TEST_CASE("sdl: MapSdlGamepad maps the buttons", "[sdl_driver]") {
  const std::pair<int, uint16_t> expected[] = {
      {0, 0x1000}, {1, 0x2000}, {2, 0x4000}, {3, 0x8000}, {4, 0x20}, {6, 0x10},  {7, 0x40},
      {8, 0x80},   {9, 0x100},  {10, 0x200}, {11, 0x1},   {12, 0x2}, {13, 0x4},  {14, 0x8}};
  for (auto [bit, xin] : expected) {
    plat::GamepadSnapshot s{};
    s.buttons = 1u << bit;
    CHECK(hid::MapSdlGamepad(s).buttons == xin);
  }
  plat::GamepadSnapshot guide{};
  guide.buttons = 1u << 5;
  CHECK(hid::MapSdlGamepad(guide).buttons == 0);
}

TEST_CASE("sdl: MapSdlGamepad axes and triggers", "[sdl_driver]") {
  plat::GamepadSnapshot s{};
  s.axes[0] = 1234;
  s.axes[1] = 32767;
  s.axes[2] = -5;
  s.axes[3] = -32768;
  s.axes[4] = 32767;
  s.axes[5] = 16384;
  auto g = hid::MapSdlGamepad(s);
  CHECK(g.thumb_lx == 1234);
  CHECK(g.thumb_ly == -32767);
  CHECK(g.thumb_rx == -5);
  CHECK(g.thumb_ry == 32767);
  CHECK(g.left_trigger == 255);
  CHECK(g.right_trigger == 127);
  s.axes[1] = -32768;
  s.axes[4] = 0;
  CHECK(hid::MapSdlGamepad(s).thumb_ly == 32767);
  CHECK(hid::MapSdlGamepad(s).left_trigger == 0);
}

TEST_CASE("sdl: slot assignment, disconnection, vibration, log", "[sdl_driver]") {
  FakeState st;
  std::vector<std::string> log;
  bool xinput0 = false;
  auto make = [&] {
    return hid::CreateSdlDriver(
        std::make_unique<FakeSource>(&st), [&](uint32_t u) { return u == 0 && xinput0; },
        [&](std::string_view l) { log.emplace_back(l); });
  };

  SECTION("a pad goes to user 0 with the mapped state") {
    auto d = make();
    CHECK(!d->GetState(0));
    auto p = Pad(10, 1);
    p.buttons = 1;
    st.pads = {p};
    auto g = d->GetState(0);
    REQUIRE(g);
    CHECK(g->buttons == 0x1000);
    CHECK(!d->GetState(1));
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "[input] SDL pad connected: DualSense (PS5) -> user 0");
  }
  SECTION("with XInput on user 0 the pad goes to user 1") {
    xinput0 = true;
    auto d = make();
    st.pads = {Pad(10, 1)};
    CHECK(!d->GetState(0));
    CHECK(d->GetState(1));
    CHECK(log.at(0) == "[input] SDL pad connected: DualSense (PS5) -> user 1");
  }
  SECTION("two pads in connection order; disconnecting frees only its own slot") {
    auto d = make();
    st.pads = {Pad(10, 1, "A", "PS5"), Pad(20, 2, "B", "PS4")};
    CHECK(d->GetState(0));
    CHECK(d->GetState(1));
    st.pads = {Pad(20, 2, "B", "PS4")};
    CHECK(!d->GetState(0));
    CHECK(d->GetState(1));
    REQUIRE(log.size() == 3);
    CHECK(log[1] == "[input] SDL pad connected: B (PS4) -> user 1");
    CHECK(log[2] == "[input] SDL pad disconnected: A (PS5) -> user 0");
  }
  SECTION("a fifth pad without a slot does not show up; it does when one frees up") {
    auto d = make();
    for (uint32_t i = 0; i < 5; ++i) st.pads.push_back(Pad(10 + i, i + 1));
    for (uint32_t u = 0; u < 4; ++u) CHECK(d->GetState(u));
    CHECK(!d->GetState(4));
    CHECK(log.size() == 4);
    st.pads.erase(st.pads.begin() + 1);  // disconnects user 1
    CHECK(d->GetState(1));               // the fifth takes the free slot
  }
  SECTION("XInput shadows an SDL pad's slot: the pad moves to the first free user") {
    auto d = make();
    st.pads = {Pad(10, 1)};
    CHECK(d->GetState(0));
    xinput0 = true;
    CHECK(!d->GetState(0));
    CHECK(d->GetState(1));
    REQUIRE(log.size() == 3);
    CHECK(log[1] == "[input] SDL pad disconnected: DualSense (PS5) -> user 0");
    CHECK(log[2] == "[input] SDL pad connected: DualSense (PS5) -> user 1");
  }
  SECTION("yielding the slot to XInput clears the pad's vibration") {
    auto d = make();
    st.pads = {Pad(10, 1)};
    CHECK(d->SetVibration(0, 500, 600));
    REQUIRE(st.rumbles.size() == 1);
    xinput0 = true;
    CHECK(!d->GetState(0));
    REQUIRE(st.rumbles.size() == 2);
    CHECK(st.rumbles[1].id == 10);
    CHECK(st.rumbles[1].low == 0);
    CHECK(st.rumbles[1].high == 0);
  }
  SECTION("with no free users the pad that yields its slot stops vibrating") {
    auto d = make();
    for (uint32_t i = 0; i < 4; ++i) st.pads.push_back(Pad(10 + i, i + 1));
    for (uint32_t u = 0; u < 4; ++u) CHECK(d->GetState(u));
    xinput0 = true;
    CHECK(!d->GetState(0));
    REQUIRE(st.rumbles.size() == 1);
    CHECK(st.rumbles[0].id == 10);
    CHECK(st.rumbles[0].low == 0);
    CHECK(st.rumbles[0].high == 0);
  }
  SECTION("XInput shadows the slot with no free users: the pad stays without a user and comes back") {
    auto d = make();
    for (uint32_t i = 0; i < 4; ++i) st.pads.push_back(Pad(10 + i, i + 1));
    for (uint32_t u = 0; u < 4; ++u) CHECK(d->GetState(u));
    xinput0 = true;
    CHECK(!d->GetState(0));
    for (uint32_t u = 1; u < 4; ++u) CHECK(d->GetState(u));
    REQUIRE(log.size() == 5);
    CHECK(log[4] == "[input] SDL pad disconnected: DualSense (PS5) -> user 0");
    xinput0 = false;
    CHECK(d->GetState(0));
    CHECK(log.size() == 6);
    CHECK(log[5] == "[input] SDL pad connected: DualSense (PS5) -> user 0");
  }
  SECTION("SetVibration uses the instance_id of the user's pad") {
    auto d = make();
    st.pads = {Pad(10, 1), Pad(20, 2)};
    CHECK(d->SetVibration(1, 100, 200));
    REQUIRE(st.rumbles.size() == 1);
    CHECK(st.rumbles[0].id == 20);
    CHECK(st.rumbles[0].low == 100);
    CHECK(st.rumbles[0].high == 200);
    CHECK(!d->SetVibration(2, 1, 1));
    CHECK(!d->SetVibration(7, 1, 1));
    CHECK(st.rumbles.size() == 1);
  }
}
