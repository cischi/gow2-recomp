// Game without patches or [engine] (tests/fixtures/unavailable_patches.cpp in place of gow2_game):
// Apply skips the entries and the launcher disables their options.
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

#include "game_patches/game_patches.h"
#include "launcher/launcher_model.h"

using namespace game_patches;

TEST_CASE("game_patches: an unavailable patch is skipped", "[game_patches]") {
  GuestMemory mem;
  REQUIRE(mem.Init());
  REQUIRE(All().size() == size_t(kPatchCount));
  for (int i = 0; i < kPatchCount; ++i) CHECK_FALSE(Available(i));
  CHECK(Apply(mem, {}).empty());  // not requested: no log
  const auto log = Apply(mem, {"show_fps"});
  CHECK(log == std::vector<std::string>{"[patch] show_fps: not available for this game"});
  CHECK_FALSE(Verified(kShowFps));
  CHECK_FALSE(Enabled(kShowFps));
  CHECK_FALSE(SetEnabled(mem, kShowFps, true));
  // main.cpp leaves disable_motion_blur in the set when unavailable, so Apply logs it too.
  CHECK(Apply(mem, {"disable_motion_blur"}) ==
        std::vector<std::string>{"[patch] disable_motion_blur: not available for this game"});
}

TEST_CASE("launcher: UnavailableOptions with no patch available", "[launcher]") {
  const std::set<std::string> expected{"show_fps", "disable_motion_blur", "disable_lens_flares", "disable_bloom",
                                       "aspect", "frame_rate", "engine", "hdr"};  // hdr: no post capture
  CHECK(launcher::UnavailableOptions() == expected);
}
