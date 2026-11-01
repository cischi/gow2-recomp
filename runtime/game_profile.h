// Per-game profile: identity and folders of the game this executable runs. Get() is defined by the
// games/<id> library linked into the executable (gow2_game for gow2.exe).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace game_profile {

// Draws of the game's post processing read by the GPU backend (gpu/d3d12_backend.cpp) for the upscaler, frame
// generation, HDR and disable_motion_blur through shader constants. *_ps: pixel shader ucode hashes (several parts
// may be the same draw); *_tf: texture fetch constants; *_c: pixel shader float constants (c<n>). A frame's
// capture is complete at the final draw if the depth and motion draws came before it in the same frame.
struct PostCapture {
  static constexpr uint8_t kNone = 0xFF;
  // Last post draw before the HUD: render target 0 after it is the image without HUD; `size_tf` gives the render
  // size and the pitch of the scene surfaces (jitter); a 1x1 `size_tf` is not a view (skipped with all its parts).
  // 0 = no capture: upscaler, frame generation and HDR not available, disable_motion_blur through the patch hooks.
  uint64_t final_ps = 0;
  uint8_t size_tf = 0;
  // Scene depth (k_24_8_FLOAT) and its linearization register: view depth = 1 / (z * c.z - c.w).
  uint64_t depth_ps = 0;
  uint8_t depth_tf = 0, linearize_c = 0;
  // Velocity buffer of the dynamic objects: screen velocity in .xy, encoded 0.5 + v / 2 with x > 0 only where an
  // object was drawn; `velocity_fetch_swizzle`: .xy after the fetch constant's swizzle (false: the host texture's).
  uint64_t motion_ps = 0;
  uint8_t velocity_tf = 0;
  bool velocity_fetch_swizzle = false;
  // Camera reprojection: 4 registers, current screen (NDC x, y times view depth, depth, 1) -> previous screen
  // (components x, y, w of each register).
  uint8_t reprojection_c = 0;
  // Velocity scale (NDC -> velocity units): .xy of `blur_scale_c`; .zw of `blur_limit_c` (kNone = 1): the velocity
  // buffer holds velocity * scale / limit. GoW2: UV blur scale and UV limit of the same register.
  uint8_t blur_scale_c = 0, blur_limit_c = kNone;
  // disable_motion_blur: these float constant components (4 * register + component) of the motion draw at 0.
  uint16_t blur_off[4] = {};
  // HDR10: gpu/hdr_passes.h reconstructs the light from this final draw (scene and bloom fetch constants);
  // false = HDR not available.
  bool hdr = false;
  uint8_t hdr_scene_tf = 0, hdr_bloom_tf = 0;
};

struct Profile {
  std::string_view id;             // "gow2": base of the exe/log/settings names, --game of tools/gow2.py
  std::string_view name;           // "Gears of War 2"
  std::string_view edition;        // "PAL Europe": ISO messages of the launcher
  std::string_view edition_it;     // "PAL Europa": the same in Italian
  std::string_view image_sha256;   // lowercase hex SHA-256 of the decrypted XEX image, games/<id>/expected_xex.json
  std::string_view data_dir;       // "game_data"
  std::string_view user_data_dir;  // "user_data"
  std::string_view cooked_dir;     // "GearGame/CookedXenon"; empty = [engine] not available
  PostCapture post;                // GPU capture of the post processing; default = none
  // Pixel shader ucode hash of the 4x4 depth-of-field downsample replaced by gpu/shader_overrides.cpp; 0 = none.
  uint64_t dof_downsample_hash = 0;
  // Submission every 100 draws also at internal resolution > 1 (patch 0020): where the GPU waits for the CPU there
  // (GoW3 3x: ~4.5 ms idle per frame on the occlusion query wait). false: only at 1x (GoW2: blue armor at 3x).
  bool submit_split_scaled = false;
  // Pixel shader ucode hash of the 2x2 depth-of-field downsample supersampled at the internal resolution by
  // gpu/shader_overrides.cpp (its half-resolution blend mask draws stair steps around bright backgrounds); 0 = none.
  uint64_t dof_downsample_2x2_hash = 0;
  // Pixel shader ucode hash of the final post-process whose blend with the guest-resolution depth-of-field buffer
  // gpu/shader_overrides.cpp upsamples along the scene's edges at the internal resolution; 0 = none.
  uint64_t dof_composite_hash = 0;
  // Pixel shader ucode hash of the screen-space shadow projection whose PCF gpu/shader_overrides.cpp replaces with a
  // wider filter on the host texels (the game's 3x3 taps leave stair steps on magnified receivers); 0 = none.
  uint64_t shadow_projection_hash = 0;
  // Pixel shader ucode hashes of the shadow projections of spot and point lights given the same filter; 0 = none.
  uint64_t shadow_spot_light_hash = 0;
  uint64_t shadow_point_light_hash = 0;
  // Pixel shader ucode hash of the depth-of-field blur downsample whose taps gpu/shader_overrides.cpp weights one by
  // one by their blurriness (in-focus silhouettes no longer bleed into the blurred background as a halo); 0 = none.
  uint64_t dof_blur_downsample_hash = 0;
};

const Profile& Get();  // defined in games/<id>/profile.cpp
inline std::string SettingsFileName() { return std::string(Get().id) + ".toml"; }
inline std::string LogFileName() { return std::string(Get().id) + ".log"; }

}  // namespace game_profile
