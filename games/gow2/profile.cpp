// Gears of War 2 (PAL Europe) profile.
#include "game_profile.h"

namespace game_profile {

const Profile& Get() {
  // UE3's uber post B6B6DD4E7D91F3D9 does everything in one draw (motion blur, depth of field, tonemapping):
  // tf0 depth, tf1 scene, tf2 bloom/DoF, tf3 velocity (k_16_16); c2 depth linearization, c13..c16
  // reprojection, c17 velocity scale (.xy) and limit (.zw), c18.xy the dynamic objects' scale (instruction 14).
  static constexpr PostCapture kPost{.final_ps = 0xB6B6DD4E7D91F3D9ull,
                                     .size_tf = 0,
                                     .depth_ps = 0xB6B6DD4E7D91F3D9ull,
                                     .depth_tf = 0,
                                     .linearize_c = 2,
                                     .motion_ps = 0xB6B6DD4E7D91F3D9ull,
                                     .velocity_tf = 3,
                                     .velocity_fetch_swizzle = false,
                                     .reprojection_c = 13,
                                     .blur_scale_c = 17,
                                     .blur_limit_c = 17,
                                     .blur_off = {17 * 4, 17 * 4 + 1, 18 * 4, 18 * 4 + 1},
                                     .hdr = true,
                                     .hdr_scene_tf = 1,
                                     .hdr_bloom_tf = 2};
  static constexpr Profile kProfile{"gow2",      "Gears of War 2",       "PAL Europe", "PAL Europa", GAME_XEX_SHA256,
                                    "game_data", "user_data", "GearGame/CookedXenon",
                                    kPost,       0x0354AF5BA0F333D1ull};
  return kProfile;
}

}  // namespace game_profile
