// Game patch table: verification at startup on a fake guest memory. The "original" bytes
// are built by the test; no game data.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>
#include <string>

#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED  // like kernel/function.h: ppc_context.h without the generated ppc_config.h
#endif
#include <ppc_context.h>

#include "game_patches/game_patches.h"
#include "settings.h"

using namespace game_patches;

namespace {
// Memory with the patches' pages committed (zero) and the original words/bytes written by the test.
struct Fixture {
  GuestMemory mem;
  Fixture() {
    REQUIRE(mem.Init());
    REQUIRE(mem.Commit(0x83123000, 0x1000));
    REQUIRE(mem.Commit(0x82000000, 0x900000));  // tile tables (0x82094010), aspect data (0x821D0B58), hooks
    REQUIRE(mem.Commit(0x8298C000, 0x1000));
    REQUIRE(mem.Commit(0x82AA8000, 0x1000));
    for (const Patch& p : All()) {
      for (const DataWrite& w : p.data) std::memcpy(mem.base + w.address, w.original.data(), w.original.size());
      for (const CodeHook& h : p.hooks) {
        const uint32_t be = __builtin_bswap32(h.original);
        std::memcpy(mem.base + h.address, &be, 4);
      }
    }
  }
};
bool Has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v) if (x == s) return true;
  return false;
}
}  // namespace

TEST_CASE("game_patches: data patch turned on", "[game_patches]") {
  Fixture f;
  const auto log = Apply(f.mem, {"show_fps"});
  CHECK(Has(log, "[patch] show_fps: turned on"));
  CHECK(f.mem.base[0x83123468] == 1);
  CHECK(f.mem.base[0x83123470] == 1);
  CHECK(Enabled(kShowFps));
  CHECK_FALSE(Enabled(kDisableBloom));
}

TEST_CASE("game_patches: different original bytes leave the memory untouched", "[game_patches]") {
  Fixture f;
  f.mem.base[0x83123470] = 0x55;  // the second does not match: the first must not be written either
  const auto log = Apply(f.mem, {"show_fps"});
  CHECK(Has(log, "[patch] show_fps: unexpected bytes at 0x83123470, skipped"));
  CHECK(f.mem.base[0x83123468] == 0);
  CHECK(f.mem.base[0x83123470] == 0x55);
  CHECK_FALSE(Enabled(kShowFps));
}

TEST_CASE("game_patches: a hook with a different instruction disables the patch", "[game_patches]") {
  Fixture f;
  f.mem.base[0x824ED078] ^= 0xFF;
  const auto log = Apply(f.mem, {"disable_bloom", "disable_lens_flares"});
  CHECK_FALSE(Enabled(kDisableBloom));
  CHECK(Enabled(kDisableLensFlares));
  bool found = false;
  for (const auto& l : log)
    found |= l.rfind("[patch] disable_bloom: ", 0) == 0 && l.find("0x824ED078") != std::string::npos;
  CHECK(found);
  CHECK(Has(log, "[patch] disable_lens_flares: turned on"));
}

TEST_CASE("game_patches: patch not requested, nothing written", "[game_patches]") {
  Fixture f;
  CHECK(Apply(f.mem, {}).empty());
  CHECK(f.mem.base[0x83123468] == 0);
  for (int i = 0; i < kPatchCount; ++i) CHECK_FALSE(Enabled(i));
}

TEST_CASE("game_patches: table order and settings keys", "[game_patches]") {
  const auto all = All();
  REQUIRE(all.size() == size_t(kPatchCount));
  for (int i = 0; i < kPatchCount; ++i) CHECK(Available(i));  // GoW2: every patch exists
  CHECK(std::string(all[kShowFps].key) == "show_fps");
  CHECK(std::string(all[kDisableMotionBlur].key) == "disable_motion_blur");
  CHECK(std::string(all[kDisableLensFlares].key) == "disable_lens_flares");
  CHECK(std::string(all[kDisableBloom].key) == "disable_bloom");
  std::set<std::string> keys, expected;
  for (const Patch& p : all) if (!p.internal) keys.insert(p.key);
  for (const auto k : settings::kPatchKeys) expected.insert(std::string(k));
  CHECK(keys == expected);
  for (const PatchId id : {kFrameRateInterval, kFrameRateUnlock}) {
    CHECK(all[id].internal);
    CHECK(std::string(all[id].authors) == "illusion, boma");
  }
  CHECK(std::string(all[kFrameRateInterval].key) == kFrameRateIntervalKey);
  CHECK(std::string(all[kFrameRateUnlock].key) == kFrameRateUnlockKey);
  CHECK(std::string(kFrameRateIntervalKey) == "frame_rate.interval");
  CHECK(std::string(kFrameRateUnlockKey) == "frame_rate.unlock");
  CHECK(all[kCoalescedNoHashCheck].internal);
  CHECK(std::string(all[kCoalescedNoHashCheck].key) == kCoalescedNoHashCheckKey);
  CHECK(std::string(kCoalescedNoHashCheckKey) == "coalesced.no_hash_check");
  CHECK(std::string(all[kCoalescedNoHashCheck].authors) == "Sowa_95, boma");
  CHECK(all[kCoalescedNoHashCheck].hooks.size() == 2);
  CHECK(all[kAspect21x9].internal);
  CHECK(all[kAspect16x10].internal);
  CHECK(std::string(all[kAspect21x9].key) == kAspect21x9Key);
  CHECK(std::string(all[kAspect16x10].key) == kAspect16x10Key);
  CHECK(std::string(kAspect21x9Key) == "aspect.21_9");
  CHECK(std::string(kAspect16x10Key) == "aspect.16_10");
  CHECK(std::string(all[kAspect21x9].authors) == "boma, TudorHorse");
  CHECK(std::string(all[kAspect16x10].authors) == "boma, TudorHorse");
  CHECK(all[kAspect21x9].hooks.size() == 4);
  CHECK(all[kAspect16x10].hooks.size() == 2);
  CHECK(all[kAspect21x9].data.size() == 6);
  CHECK(all[kAspect16x10].data.size() == 7);
}

bool Gow2PatchCoalescedHash1(PPCRegister& r3);
bool Gow2PatchCoalescedHash2();

TEST_CASE("game_patches: internal patch of the Coalesced check", "[game_patches]") {
  {
    Fixture f;
    const auto log = Apply(f.mem, {"coalesced.no_hash_check"});
    CHECK(Has(log, "[patch] coalesced.no_hash_check: turned on"));
    CHECK(Enabled(kCoalescedNoHashCheck));
  }
  {
    Fixture f;
    const uint32_t be = __builtin_bswap32(0x60000000);
    std::memcpy(f.mem.base + 0x827F9148, &be, 4);
    const auto log = Apply(f.mem, {"coalesced.no_hash_check"});
    CHECK(Has(log, "[patch] coalesced.no_hash_check: unexpected instruction at 0x827F9148 (Gow2PatchCoalescedHash2), skipped"));
    CHECK_FALSE(Enabled(kCoalescedNoHashCheck));
  }
  Fixture f;
  PPCRegister r{};
  r.u64 = 0xFFFFFFFF00000001ull;
  Apply(f.mem, {});
  CHECK_FALSE(Gow2PatchCoalescedHash1(r));
  CHECK(r.u64 == 0xFFFFFFFF00000001ull);
  CHECK_FALSE(Gow2PatchCoalescedHash2());
  Apply(f.mem, {"coalesced.no_hash_check"});
  CHECK(Gow2PatchCoalescedHash1(r));
  CHECK(r.u64 == 0);
  CHECK(Gow2PatchCoalescedHash2());
}

TEST_CASE("game_patches: internal frame rate patches", "[game_patches]") {
  {
    Fixture f;
    const auto log = Apply(f.mem, {"frame_rate.interval"});
    CHECK(Has(log, "[patch] frame_rate.interval: turned on"));
    CHECK(Enabled(kFrameRateInterval));
    CHECK_FALSE(Enabled(kFrameRateUnlock));
  }
  {
    Fixture f;
    const uint32_t be = __builtin_bswap32(0x60000000);
    std::memcpy(f.mem.base + 0x824A2E94, &be, 4);
    const auto log = Apply(f.mem, {"frame_rate.interval", "frame_rate.unlock"});
    CHECK(Has(log, "[patch] frame_rate.unlock: unexpected instruction at 0x824A2E94 (Gow2PatchUnlockFps), skipped"));
    CHECK_FALSE(Enabled(kFrameRateUnlock));
    CHECK(Enabled(kFrameRateInterval));
  }
  {
    Fixture f;
    Apply(f.mem, {"frame_rate.interval", "frame_rate.unlock"});
    REQUIRE(Enabled(kFrameRateUnlock));
    Disable(kFrameRateUnlock);
    CHECK_FALSE(Enabled(kFrameRateUnlock));
    CHECK(Enabled(kFrameRateInterval));
  }
}

TEST_CASE("game_patches: a second Apply clears the flags of the first", "[game_patches]") {
  Fixture f;
  Apply(f.mem, {"show_fps"});
  REQUIRE(Enabled(kShowFps));
  Apply(f.mem, {});
  CHECK_FALSE(Enabled(kShowFps));
}

// Hooks of the recompiled code (declared extern by the generated code). Off: false and register
// untouched; on: true and the u64 register set to 0 as `li` would do.
bool Gow2PatchMotionBlur(PPCRegister& r11);
bool Gow2PatchLensFlares(PPCRegister& r10);
bool Gow2PatchBloom1(PPCRegister& r10);
bool Gow2PatchBloom2(PPCRegister& r11);
bool Gow2PatchBloom3(PPCRegister& r10);

namespace {
template <class F>
void CheckHook(F hook, const char* key) {
  Fixture f;
  Apply(f.mem, {});
  PPCRegister r{};
  r.u64 = 0x123456789ABCDEF0ull;
  CHECK_FALSE(hook(r));
  CHECK(r.u64 == 0x123456789ABCDEF0ull);
  Apply(f.mem, {key});
  CHECK(hook(r));
  CHECK(r.u64 == 0);
}
}  // namespace

bool Gow2PatchPresentInterval(PPCRegister& r11);
bool Gow2PatchUnlockFps(PPCRegister& f1);

TEST_CASE("game_patches: frame rate hooks", "[game_patches]") {
  Fixture f;
  PPCRegister r{};
  r.u64 = 0x123456789ABCDEF0ull;
  Apply(f.mem, {});
  CHECK_FALSE(Gow2PatchPresentInterval(r));
  CHECK_FALSE(Gow2PatchUnlockFps(r));
  CHECK(r.u64 == 0x123456789ABCDEF0ull);
  Apply(f.mem, {"frame_rate.interval", "frame_rate.unlock"});
  SetPresentInterval(0);
  CHECK(Gow2PatchPresentInterval(r));
  CHECK(r.u64 == 0);
  SetPresentInterval(1);
  CHECK(Gow2PatchPresentInterval(r));
  CHECK(r.u64 == 1);
  r.u64 = 0x123456789ABCDEF0ull;
  CHECK(Gow2PatchUnlockFps(r));
  CHECK(r.u64 == 0x123456789ABCDEF0ull);
}

TEST_CASE("game_patches: code hooks", "[game_patches]") {
  CheckHook(Gow2PatchMotionBlur, "disable_motion_blur");
  CheckHook(Gow2PatchLensFlares, "disable_lens_flares");
  CheckHook(Gow2PatchBloom1, "disable_bloom");
  CheckHook(Gow2PatchBloom2, "disable_bloom");
  CheckHook(Gow2PatchBloom3, "disable_bloom");
}

bool Gow2PatchNoConstrainAspect();
bool Gow2PatchUwFov(PPCRegister& f0, PPCRegister& f30);
bool Gow2PatchSceneHeight(PPCRegister& r11);
bool Gow2PatchUwUiHeight(PPCRegister& r3);

namespace {
uint32_t Word(const Fixture& f, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, f.mem.base + a, 4);
  return v;
}
constexpr uint64_t kPoison = 0x123456789ABCDEF0ull;
// Bottoms of the tile rectangles written by the two aspect entries.
constexpr uint32_t kTileBottoms[] = {0x8209403C, 0x8209405C, 0x8209408C, 0x820940CC, 0x820940DC};
}  // namespace

TEST_CASE("game_patches: aspect entries", "[game_patches]") {
  const auto be = [](uint32_t v) { return __builtin_bswap32(v); };
  {
    Fixture f;
    const auto log = Apply(f.mem, {"aspect.21_9"});
    CHECK(Has(log, "[patch] aspect.21_9: turned on"));
    CHECK(Enabled(kAspect21x9));
    CHECK(Word(f, 0x821D0B28) == be(0x40155555));
    for (const uint32_t a : kTileBottoms) CHECK(Word(f, a) == be(550));
  }
  {
    Fixture f;
    Apply(f.mem, {"aspect.16_10"});
    CHECK(Enabled(kAspect16x10));
    CHECK(Word(f, 0x821D0B28) == be(0x3FCCCCCD));
    CHECK(Word(f, 0x821D0B58) == be(0x44480000));  // 800.0f
    for (const uint32_t a : kTileBottoms) CHECK(Word(f, a) == be(800));
  }
  {
    Fixture f;
    const auto log = Apply(f.mem, {"aspect.21_9", "aspect.16_10"});
    CHECK(Enabled(kAspect21x9));
    CHECK(Has(log, "[patch] aspect.16_10: unexpected bytes at 0x821D0B28, skipped"));
    CHECK_FALSE(Enabled(kAspect16x10));
  }
  {
    Fixture f;
    f.mem.base[0x820940DF] ^= 0xFF;  // last bottom different: no byte of the entry must be written
    const auto log = Apply(f.mem, {"aspect.21_9"});
    CHECK(Has(log, "[patch] aspect.21_9: unexpected bytes at 0x820940DC, skipped"));
    CHECK_FALSE(Enabled(kAspect21x9));
    CHECK(Word(f, 0x821D0B28) == be(0x3FE38E39));
    CHECK(Word(f, 0x8209403C) == be(720));
  }
  {
    Fixture f;
    const uint32_t nop = be(0x60000000);
    std::memcpy(f.mem.base + 0x82806B10, &nop, 4);
    const auto log = Apply(f.mem, {"aspect.16_10"});
    CHECK(Has(log, "[patch] aspect.16_10: unexpected instruction at 0x82806B10 (Gow2PatchSceneHeight), skipped"));
    CHECK_FALSE(Enabled(kAspect16x10));
    CHECK(Word(f, 0x821D0B58) == be(0x44340000));
  }
}

TEST_CASE("game_patches: aspect hooks", "[game_patches]") {
  Fixture f;
  PPCRegister r11{}, r3{};
  auto reset = [&] { r11.u64 = r3.u64 = kPoison; };
  reset();
  Apply(f.mem, {});
  CHECK_FALSE(Gow2PatchNoConstrainAspect());
  CHECK_FALSE(Gow2PatchSceneHeight(r11));
  CHECK_FALSE(Gow2PatchUwUiHeight(r3));
  CHECK((r11.u64 == kPoison && r3.u64 == kPoison));

  Apply(f.mem, {"aspect.21_9"});
  reset();
  CHECK(Gow2PatchNoConstrainAspect());
  CHECK(Gow2PatchSceneHeight(r11));
  CHECK(r11.u64 == 550);
  CHECK(Gow2PatchUwUiHeight(r3));
  CHECK(r3.u64 == 564);

  Fixture g;  // 21:9 already wrote the bytes of 0x821D0B28 in f
  Apply(g.mem, {"aspect.16_10"});
  reset();
  CHECK(Gow2PatchNoConstrainAspect());
  CHECK(Gow2PatchSceneHeight(r11));
  CHECK(r11.u64 == 800);
  CHECK_FALSE(Gow2PatchUwUiHeight(r3));
  CHECK(r3.u64 == kPoison);
  PPCRegister f0{}, f30{};
  f30.f64 = 70;
  f0.f64 = std::bit_cast<double>(kPoison);
  CHECK_FALSE(Gow2PatchUwFov(f0, f30));
  CHECK(std::bit_cast<uint64_t>(f0.f64) == kPoison);
}

TEST_CASE("game_patches: 21:9 FOV from the tangent", "[game_patches]") {
  Fixture f;
  Apply(f.mem, {"aspect.21_9"});
  for (const double fov : {40.0, 60.0, 90.0}) {
    PPCRegister f0{}, f30{};
    f30.f64 = fov;
    REQUIRE(Gow2PatchUwFov(f0, f30));
    CHECK(f0.f64 == double(float(f0.f64)));  // like an lfs
    const double ratio = std::tan(fov * f0.f64) / std::tan(fov * std::numbers::pi / 360);
    CHECK(ratio == Catch::Approx((1280.0 / 550) / (16.0 / 9)).epsilon(1e-4));  // 1.30909: Hor+
  }
  for (const double bad : {0.0, -10.0, 180.0, 200.0, std::nan("")}) {
    PPCRegister f0{}, f30{};
    f0.f64 = 1.5;
    f30.f64 = bad;
    CHECK_FALSE(Gow2PatchUwFov(f0, f30));
    CHECK(f0.f64 == 1.5);
  }
  Apply(f.mem, {});
  PPCRegister f0{}, f30{};
  f30.f64 = 60.0;
  CHECK_FALSE(Gow2PatchUwFov(f0, f30));
}

TEST_CASE("game_patches: Apply also verifies patches that are off", "[game_patches]") {
  {
    Fixture f;
    CHECK(Apply(f.mem, {}).empty());
    CHECK(Verified(kShowFps));
    CHECK(Verified(kDisableBloom));
    CHECK_FALSE(Enabled(kShowFps));
    CHECK_FALSE(Enabled(kDisableBloom));
    CHECK(f.mem.base[0x83123468] == 0);
    CHECK(f.mem.base[0x83123470] == 0);
  }
  Fixture f;
  f.mem.base[0x827B817C] ^= 0xFF;  // word of Gow2PatchLensFlares different
  const auto log = Apply(f.mem, {});
  CHECK_FALSE(Verified(kDisableLensFlares));
  bool found = false;
  for (const auto& l : log) found |= l.find("disable_lens_flares: unexpected instruction") != std::string::npos;
  CHECK(found);
}

TEST_CASE("game_patches: SetEnabled turns a data patch on and off", "[game_patches]") {
  Fixture f;
  Apply(f.mem, {});
  CHECK(SetEnabled(f.mem, kShowFps, true));
  CHECK(f.mem.base[0x83123468] == 0x01);
  CHECK(f.mem.base[0x83123470] == 0x01);
  CHECK(Enabled(kShowFps));
  CHECK(SetEnabled(f.mem, kShowFps, false));
  CHECK(f.mem.base[0x83123468] == 0x00);
  CHECK(f.mem.base[0x83123470] == 0x00);
  CHECK_FALSE(Enabled(kShowFps));
}

TEST_CASE("game_patches: SetEnabled on hooks", "[game_patches]") {
  Fixture f;
  Apply(f.mem, {});
  CHECK(SetEnabled(f.mem, kDisableBloom, true));
  CHECK(Enabled(kDisableBloom));
  CHECK(Word(f, 0x824ECEF8) == __builtin_bswap32(0x814A0038));
}

TEST_CASE("game_patches: SetEnabled rejects internal and unverified patches", "[game_patches]") {
  Fixture f;
  f.mem.base[0x827B817C] ^= 0xFF;
  Apply(f.mem, {});
  CHECK_FALSE(SetEnabled(f.mem, kFrameRateUnlock, true));
  CHECK_FALSE(Enabled(kFrameRateUnlock));
  CHECK_FALSE(SetEnabled(f.mem, kDisableLensFlares, true));
  CHECK_FALSE(Enabled(kDisableLensFlares));
}
