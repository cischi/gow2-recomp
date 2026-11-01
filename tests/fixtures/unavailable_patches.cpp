// Test game where no patch is available: Table() with the canonical keys and no data/hooks, profile
// without cooked_dir ([engine] not available). Linked in place of gow2_game.
#include "game_patches/game_patches.h"
#include "game_profile.h"

namespace game_patches {

std::span<const Patch> Table() {
  static const std::vector<Patch> t = {
      {"show_fps", "", {}, {}, false, false},
      {"disable_motion_blur", "", {}, {}, false, false},
      {"disable_lens_flares", "", {}, {}, false, false},
      {"disable_bloom", "", {}, {}, false, false},
      {kFrameRateIntervalKey, "", {}, {}, true, false},
      {kFrameRateUnlockKey, "", {}, {}, true, false},
      {kCoalescedNoHashCheckKey, "", {}, {}, true, false},
      {kAspect21x9Key, "", {}, {}, true, false},
      {kAspect16x10Key, "", {}, {}, true, false},
  };
  return t;
}

}  // namespace game_patches

namespace game_profile {

const Profile& Get() {
  static constexpr Profile kProfile{"test", "Test game", "Test", "Test", "", "game_data", "user_data", ""};
  return kProfile;
}

}  // namespace game_profile
