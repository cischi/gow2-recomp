// Gears of War 2 patch table and the Gow2Patch* hooks of games/gow2/gow2.toml.
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED  // like kernel/function.h: ppc_context.h without the generated ppc_config.h
#endif
#include <ppc_context.h>

#include "game_patches/game_patches.h"

#include <cmath>

namespace game_patches {
namespace {

// Bottoms of the tile rectangles (big-endian D3DRECT, 0x82094010..0x820940DC, bottom 720) set to `rows`: the
// predicated tiling of 2x MSAA resolves down to the bottom, past the scene if it is shorter (aspect-tiling spike).
std::vector<DataWrite> WithTileRows(std::vector<DataWrite> data, uint16_t rows) {
  for (const uint32_t a : {0x8209403Cu, 0x8209405Cu, 0x8209408Cu, 0x820940CCu, 0x820940DCu})
    data.push_back({a, {0x00, 0x00, 0x02, 0xD0}, {0x00, 0x00, uint8_t(rows >> 8), uint8_t(rows & 0xFF)}});
  return data;
}

}  // namespace

std::span<const Patch> Table() {
  // Every CodeHook on ONE line: tests/test_game_patches_config.py reads this file with a regex on {"Gow2Patch...", 0x...
  static const std::vector<Patch> t = {
      // show_fps: Sowa_95
      {"show_fps", "Sowa_95", {{0x83123468, {0x00}, {0x01}}, {0x83123470, {0x00}, {0x01}}}, {}},
      // disable_motion_blur: illusion (lwz r11,44(r10) -> li r11,0)
      {"disable_motion_blur", "illusion", {}, {
          {"Gow2PatchMotionBlur", 0x8236004C, 0x816A002C},
      }},
      // disable_lens_flares: boma (lwz r10,72(r11) -> li r10,0)
      {"disable_lens_flares", "boma", {}, {
          {"Gow2PatchLensFlares", 0x827B817C, 0x814B0048},
      }},
      // disable_bloom: boma
      {"disable_bloom", "boma", {}, {
          {"Gow2PatchBloom1", 0x824ECEF8, 0x814A0038},
          {"Gow2PatchBloom2", 0x824ED078, 0x816A0038},
          {"Gow2PatchBloom3", 0x824ED0A4, 0x814B0038},
      }},
      // frame_rate (internal, from [game] frame_rate): illusion, boma (li r11,2 -> li r11,<interval>)
      {"frame_rate.interval", "illusion, boma", {}, {
          {"Gow2PatchPresentInterval", 0x8298CFB0, 0x39600002},
      }, true},
      // frame_rate (internal): Unlock FPS by illusion and boma (lfs f1,0x504(r3) skipped, GetMaxTickRate without limit)
      {"frame_rate.unlock", "illusion, boma", {}, {
          {"Gow2PatchUnlockFps", 0x824A2E94, 0xC0230504},
      }, true},
      // coalesced.no_hash_check (internal, from [engine]): "Disable Coalesced Hash Check" by Sowa_95, boma
      // (li r3,1 -> li r3,0; mflr r12 at the start of the function -> blr)
      {"coalesced.no_hash_check", "Sowa_95, boma", {}, {
          {"Gow2PatchCoalescedHash1", 0x82AA8904, 0x38600001},
          {"Gow2PatchCoalescedHash2", 0x827F9148, 0x7D8802A6},
      }, true},
      // aspect.21_9 (internal, from [video] aspect): "21:9 Ultrawide Aspect Ratio Support" by boma, TudorHorse; Xenia's
      // code caves sit on live code and here become C++ hooks. Tile bottoms at 550: 2x MSAA as at 16:9.
      {"aspect.21_9", "boma, TudorHorse", WithTileRows({{0x821D0B28, {0x3F, 0xE3, 0x8E, 0x39}, {0x40, 0x15, 0x55, 0x55}}}, 550), {
          {"Gow2PatchNoConstrainAspect", 0x8252D608, 0x419A0094},
          {"Gow2PatchUwFov", 0x8252D73C, 0xC0070408},
          {"Gow2PatchSceneHeight", 0x82806B10, 0x396002D0},
          {"Gow2PatchUwUiHeight", 0x826E3720, 0x80E8000C},
      }, true},
      // aspect.16_10 (internal): 1280x800 Vert+ with MSAA. Ratio 1.6 and ConstrainAspectRatio from "Steam Deck / 16:10
      // Aspect Ratio Support" by boma, TudorHorse; reference height of the width 720.0 -> 800.0 (1.6 x 800 =
      // 1280: below 1280 the engine turns tiling off), scene and tiles at 800. No FOV or UIScene hook.
      {"aspect.16_10", "boma, TudorHorse", WithTileRows({{0x821D0B28, {0x3F, 0xE3, 0x8E, 0x39}, {0x3F, 0xCC, 0xCC, 0xCD}},
                                                         {0x821D0B58, {0x44, 0x34, 0x00, 0x00}, {0x44, 0x48, 0x00, 0x00}}}, 800), {
          {"Gow2PatchNoConstrainAspect", 0x8252D608, 0x419A0094},
          {"Gow2PatchSceneHeight", 0x82806B10, 0x396002D0},
      }, true},
  };
  return t;
}

}  // namespace game_patches

// [[midasm_hook]] hooks of games/gow2/gow2.toml (jump_address_on_true = address + 4, except
// Gow2PatchNoConstrainAspect and Gow2PatchUwUiHeight which jump further ahead): declared
// extern by the generated code, global C++ linkage. True = patch on: the register takes the
// patch's value (0, scene height, ...) and the original instruction is skipped; false =
// nothing happens and the original runs.
static bool ZeroIf(game_patches::PatchId id, PPCRegister& r) {
  if (!game_patches::Enabled(id)) return false;
  r.u64 = 0;
  return true;
}
bool Gow2PatchMotionBlur(PPCRegister& r11) { return ZeroIf(game_patches::kDisableMotionBlur, r11); }
bool Gow2PatchLensFlares(PPCRegister& r10) { return ZeroIf(game_patches::kDisableLensFlares, r10); }
bool Gow2PatchBloom1(PPCRegister& r10) { return ZeroIf(game_patches::kDisableBloom, r10); }
bool Gow2PatchBloom2(PPCRegister& r11) { return ZeroIf(game_patches::kDisableBloom, r11); }
bool Gow2PatchBloom3(PPCRegister& r10) { return ZeroIf(game_patches::kDisableBloom, r10); }

// Frame rate: `li r11,2` becomes `li r11,<interval>`; Unlock FPS skips the lfs (f1 stays as it is).
bool Gow2PatchPresentInterval(PPCRegister& r11) {
  if (!game_patches::Enabled(game_patches::kFrameRateInterval)) return false;
  r11.u64 = game_patches::PresentInterval();
  return true;
}
bool Gow2PatchUnlockFps(PPCRegister&) { return game_patches::Enabled(game_patches::kFrameRateUnlock); }

// Coalesced hash: `li r3,1` becomes `li r3,0`; the second returns immediately (blr in place of mflr r12).
bool Gow2PatchCoalescedHash1(PPCRegister& r3) { return ZeroIf(game_patches::kCoalescedNoHashCheck, r3); }
bool Gow2PatchCoalescedHash2() { return game_patches::Enabled(game_patches::kCoalescedNoHashCheck); }

// Aspect (21:9 and 16:10). ConstrainAspectRatio = FALSE: beq -> b 0x8252D69C, common to both entries.
bool Gow2PatchNoConstrainAspect() {
  return game_patches::Enabled(game_patches::kAspect21x9) || game_patches::Enabled(game_patches::kAspect16x10);
}
// 21:9, FOV: lfs f0,0x408(r7) loads pi/360 and the caller does f1 = f30 * f0 (half angle in radians).
// Here f0 = atan(tan(f30*pi/360) * k) / f30 with k = (1280/550) / (16/9): pure Hor+ for every FOV (including
// aim zoom), whereas the fixed constant 0.0106355 of the original patch is right at one FOV only. FOV outside
// (0, 180) or NaN: false and the original lfs runs.
bool Gow2PatchUwFov(PPCRegister& f0, PPCRegister& f30) {
  if (!game_patches::Enabled(game_patches::kAspect21x9)) return false;
  const double fov = f30.f64;
  if (!(fov > 0.0 && fov < 180.0)) return false;
  constexpr double kHorPlus = (1280.0 / 550.0) / (16.0 / 9.0);
  constexpr double kPi = 3.14159265358979323846;
  f0.f64 = double(float(std::atan(std::tan(fov * kPi / 360.0) * kHorPlus) / fov));
  return true;
}
// Aspect: li r11,720 -> height of the scene render targets, 550 at 21:9 and 800 at 16:10 (one hook per address).
bool Gow2PatchSceneHeight(PPCRegister& r11) {
  if (game_patches::Enabled(game_patches::kAspect21x9)) {
    r11.u64 = 550;
  } else if (game_patches::Enabled(game_patches::kAspect16x10)) {
    r11.u64 = 800;
  } else {
    return false;
  }
  return true;
}
// 21:9: lwz r7,12(r8); mtctr r7; bctrl -> li r3,564 (UIScene viewport height = 550 + 14 overscan); jumps to
// 0x826E372C.
bool Gow2PatchUwUiHeight(PPCRegister& r3) {
  if (!game_patches::Enabled(game_patches::kAspect21x9)) return false;
  r3.u64 = 564;
  return true;
}
