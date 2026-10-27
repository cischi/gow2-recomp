// Input system (runtime/hid): xam_input.cc semantics with fake drivers and a
// real GuestMemory arena. Synthetic data only.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fake_input_driver.h"
#include "guest_memory.h"
#include "hid/input_system.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"

PPC_EXTERN_FUNC(__imp__XamInputGetState);
PPC_EXTERN_FUNC(__imp__XamInputGetCapabilities);
PPC_EXTERN_FUNC(__imp__XamInputSetState);

namespace {

using hid_test::FakeInputDriver;

struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    kernel::g_base = mem.base;
    REQUIRE(kernel::InitMemory(mem));
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = kernel::Memory()->heap.Alloc(size);
    REQUIRE(a != 0);
    std::memset(mem.base + a, 0, size);
    return a;
  }
};
Env& E() {
  static Env env;
  return env;
}

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[4] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, kernel::g_base);
  return ctx.r3.u32;
}

uint64_t Counter(const std::string& name) {
  const std::string s = kernel::stats::Summary();
  const std::string key = "[summary] " + name + "=";
  const size_t pos = s.find(key);
  REQUIRE(pos != std::string::npos);
  return std::stoull(s.substr(pos + key.size()));
}

hid::Gamepad Sample() {
  return {0x1010, 0x11, 0x22, -2, 0x1234, 0x7FFF, -32768};
}

// Installs a single fake driver; returns the pointer (owned by the system).
FakeInputDriver* InstallOne(std::vector<hid_test::Vib>* vib = nullptr) {
  auto d = std::make_unique<FakeInputDriver>();
  FakeInputDriver* raw = d.get();
  raw->vibrations = vib;
  std::vector<std::unique_ptr<hid::InputDriver>> v;
  v.push_back(std::move(d));
  hid::Init(std::move(v));
  return raw;
}

uint32_t Get32(uint32_t a) { return *kernel::GuestPtr<be<uint32_t>>(a); }

}  // namespace

TEST_CASE("input: user 0 state in big-endian memory") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(16);
  CHECK(hid::GetState(0, 1, p) == 0);
  const uint8_t expect[16] = {0, 0, 0, 1, 0x10, 0x10, 0x11, 0x22, 0xFF, 0xFE, 0x12, 0x34, 0x7F, 0xFF, 0x80, 0x00};
  CHECK(std::memcmp(e.mem.base + p, expect, 16) == 0);
  hid::Shutdown();
}

TEST_CASE("input: packet_number grows only if the state changes") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(16);
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(Get32(p) == 1);
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(Get32(p) == 1);
  d->state[0]->buttons ^= 1;
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(Get32(p) == 2);
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(Get32(p) == 2);
  hid::Shutdown();
}

TEST_CASE("input: user without a pad, memory untouched") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(16);
  std::memset(e.mem.base + p, 0xAB, 16);
  for (uint32_t u : {1u, 2u, 3u, 4u, 0x100u}) CHECK(hid::GetState(u, 1, p) == 0x48F);
  for (int i = 0; i < 16; ++i) CHECK(e.mem.base[p + i] == 0xAB);
  hid::Shutdown();
}

TEST_CASE("input: user 0xFF and XINPUT_FLAG_ANY_USER are pinned to 0") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(16);
  CHECK(hid::GetState(0xFF, 1, p) == 0);
  CHECK(Get32(p) == 1);
  std::memset(e.mem.base + p, 0, 16);
  CHECK(hid::GetState(3, hid::kFlagAnyUser | 1, p) == 0);
  CHECK(Get32(p) == 1);
  CHECK(Get32(p + 4) == 0x10101122);
  hid::Shutdown();
}

TEST_CASE("input: non-gamepad device flags") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(32);
  CHECK(hid::GetState(0, 2, p) == 0x48F);
  CHECK(hid::GetCapabilities(0, 2, p) == 0x48F);
  CHECK(hid::GetState(0, 0, p) == 0);  // flags = 0 counts as gamepad
  hid::Shutdown();
}

TEST_CASE("input: null state pointer, result only") {
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  CHECK(hid::GetState(0, 1, 0) == 0);
  CHECK(hid::GetState(1, 1, 0) == 0x48F);
  hid::Shutdown();
}

TEST_CASE("input: the first driver that answers wins") {
  Env& e = E();
  auto a = std::make_unique<FakeInputDriver>();
  auto b = std::make_unique<FakeInputDriver>();
  FakeInputDriver *pa = a.get(), *pb = b.get();
  pa->state[0] = hid::Gamepad{0x0001};
  pb->state[0] = hid::Gamepad{0x0002};
  std::vector<std::unique_ptr<hid::InputDriver>> v;
  v.push_back(std::move(a));
  v.push_back(std::move(b));
  hid::Init(std::move(v));
  const uint32_t p = e.Alloc(16);
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(*kernel::GuestPtr<be<uint16_t>>(p + 4) == 0x0001);
  pa->state[0].reset();  // the first is not connected: the second answers
  CHECK(hid::GetState(0, 1, p) == 0);
  CHECK(*kernel::GuestPtr<be<uint16_t>>(p + 4) == 0x0002);
  hid::Shutdown();
}

TEST_CASE("input: capabilities") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  CHECK(hid::GetCapabilities(0, 1, 0) == 0xA0);
  const uint32_t p = e.Alloc(20);
  CHECK(hid::GetCapabilities(0, 1, p) == 0);
  const uint8_t* m = e.mem.base + p;
  CHECK(m[0] == 1);
  CHECK(m[1] == 1);
  CHECK(m[2] == 0);
  CHECK(m[3] == 0);
  CHECK(m[4] == 0xFF);  // buttons 0xFFFF
  CHECK(m[5] == 0xFF);
  CHECK(m[6] == 0xFF);  // trigger
  CHECK(m[7] == 0xFF);
  for (int i = 0; i < 4; ++i) {
    CHECK(m[8 + 2 * i] == 0x7F);
    CHECK(m[9 + 2 * i] == 0xFF);
  }
  for (int i = 16; i < 20; ++i) CHECK(m[i] == 0xFF);
  CHECK(hid::GetCapabilities(1, 1, p) == 0x48F);
  hid::Shutdown();
}

TEST_CASE("input: vibration") {
  Env& e = E();
  std::vector<hid_test::Vib> vib;
  FakeInputDriver* d = InstallOne(&vib);
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(4);
  *kernel::GuestPtr<be<uint16_t>>(p) = 0x1234;
  *kernel::GuestPtr<be<uint16_t>>(p + 2) = 0x5678;
  CHECK(hid::SetState(0, 0, p) == 0);
  REQUIRE(vib.size() == 1);
  CHECK(vib[0].user == 0);
  CHECK(vib[0].left == 0x1234);
  CHECK(vib[0].right == 0x5678);
  CHECK(hid::SetState(0, 0, 0) == 0xA0);
  CHECK(hid::SetState(1, 0, p) == 0x48F);
  CHECK(hid::SetState(0xFF, 0, p) == 0);  // 0xFF -> user 0
  hid::Shutdown();
}

TEST_CASE("input: no driver, as before") {
  Env& e = E();
  hid::Init({});
  const uint32_t p = e.Alloc(32);
  CHECK(hid::GetState(0, 1, p) == 0x48F);
  CHECK(hid::GetState(0xFF, 1, 0) == 0x48F);
  CHECK(hid::GetCapabilities(0, 1, p) == 0x48F);
  CHECK(hid::GetCapabilities(0, 1, 0) == 0xA0);
  CHECK(hid::SetState(0, 0, p) == 0x48F);
  hid::Shutdown();
}

TEST_CASE("input: the XamInput* exports use the system and count the calls") {
  Env& e = E();
  FakeInputDriver* d = InstallOne();
  d->state[0] = Sample();
  const uint32_t p = e.Alloc(32);
  const uint64_t before = Counter("input_get_state_calls");
  CHECK(Call(__imp__XamInputGetState, {0, 1, p}) == 0);
  CHECK(Call(__imp__XamInputGetState, {1, 1, p}) == 0x48F);  // a failing result counts anyway
  CHECK(Call(__imp__XamInputGetState, {0, 1, 0}) == 0);
  CHECK(Counter("input_get_state_calls") - before == 3);
  CHECK(Call(__imp__XamInputGetCapabilities, {0, 1, p}) == 0);
  CHECK(Call(__imp__XamInputSetState, {0, 0, p}) == 0);
  CHECK(Counter("input_get_state_calls") - before == 3);  // the others do not count
  hid::Shutdown();
}

TEST_CASE("input: StopAllVibration clears every user of every driver") {
  // The first driver answers all users: none of them must stop the loop.
  std::vector<hid_test::Vib> va, vb;
  auto a = std::make_unique<FakeInputDriver>();
  auto b = std::make_unique<FakeInputDriver>();
  a->vibrations = &va;
  b->vibrations = &vb;
  for (uint32_t u = 0; u < 4; ++u) a->state[u] = b->state[u] = hid::Gamepad{};
  std::vector<std::unique_ptr<hid::InputDriver>> v;
  v.push_back(std::move(a));
  v.push_back(std::move(b));
  hid::Init(std::move(v));
  CHECK(hid::StopAllVibration());
  for (const auto* vib : {&va, &vb}) {
    REQUIRE(vib->size() == 4);
    for (uint32_t u = 0; u < 4; ++u) {
      CHECK((*vib)[u].user == u);
      CHECK((*vib)[u].left == 0);
      CHECK((*vib)[u].right == 0);
    }
  }
  hid::Shutdown();
  CHECK(hid::StopAllVibration());  // no driver: nothing to do
}
