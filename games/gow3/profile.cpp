// Gears of War 3 (World and Italy: same code, different XEX container) profile.
#include "game_profile.h"

namespace game_profile {

const Profile& Get() {
  // The post chain is split. Identified in Act 1 gameplay from a per-draw log (pixel shader hash, pitch, render
  // target format, fetch constants), Xenia's ucode disassembly of each shader, the register file per draw and a
  // readback of the captured textures: the final draw is the last full-screen one before the HUD draws, motion blur
  // is the pass whose ucode has GoW2's reprojection, and its depth/velocity inputs were traced back to their writers.
  // - F820776A22228E1A, half-resolution downsample: scene depth tf0 (k_24_8_FLOAT 1280x720), linearized with c2 as
  //   in GoW2;
  // - 63E1C54E3C4C2596, half-resolution depth of field and motion blur: velocity buffer tf1 (k_8_8_8_8 640x360,
  //   written by CFC6D41016F07AC2 for the skinned meshes only: screen velocity * c12.xy, cleared to 0; read
  //   through the fetch swizzle), camera reprojection c8..c11 with GoW2's layout, blur scale c12.xy (times c13.xy);
  // - 2F284B68A112ED3E, final: scene tf0 (1280x720), blurred tf4, bloom tf1, filmic tonemap and 3D LUT tf3; no
  //   depth or velocity. HDR: gpu/hdr_passes.h reconstructs GoW2's uber post, not this tonemapper.
  // disable_motion_blur: camera scale c12.xy and dynamic scale c13.zw of the motion draw at 0.
  static constexpr PostCapture kPost{.final_ps = 0x2F284B68A112ED3Eull,
                                     .size_tf = 0,
                                     .depth_ps = 0xF820776A22228E1Aull,
                                     .depth_tf = 0,
                                     .linearize_c = 2,
                                     .motion_ps = 0x63E1C54E3C4C2596ull,
                                     .velocity_tf = 1,
                                     .velocity_fetch_swizzle = true,
                                     .reprojection_c = 8,
                                     .blur_scale_c = 12,
                                     .blur_limit_c = PostCapture::kNone,
                                     .blur_off = {12 * 4, 12 * 4 + 1, 13 * 4 + 2, 13 * 4 + 3},
                                     .hdr = false,
                                     .hdr_scene_tf = 0,
                                     .hdr_bloom_tf = 0};
  static constexpr Profile kProfile{"gow3",           "Gears of War 3", "World / Italy", "World / Italia", GAME_XEX_SHA256,
                                    "game_data_gow3", "user_data_gow3", "GearGame/CookedXbox360", kPost,
                                    0,                true,
                                    0xF820776A22228E1Aull, 0x2F284B68A112ED3Eull, 0x3C5E875D17B49A36ull,
                                    0x857CECDEB64A657Full, 0xCA3F4A808B09D6F4ull, 0x363BCAEF3EB3423Bull};
  return kProfile;
}

}  // namespace game_profile
