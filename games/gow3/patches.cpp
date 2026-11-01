// Gears of War 3 patch table and the Gow3Patch* hooks of games/gow3/gow3.toml.
// Source: xenia-canary/game-patches, the file of title 4D5308AB for the disc default.xex (module hash
// EF4AD5E9DD1982ED, no title update). Every address was checked against our image: the original
// instruction/bytes below are the ones in build/xex_gow3/image.bin (the TU6 file's addresses do not match).
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED  // like kernel/function.h: ppc_context.h without the generated ppc_config.h
#endif
#include <ppc_context.h>

#include "game_patches/game_patches.h"

#include <cmath>

namespace game_patches {

std::span<const Patch> Table() {
  // Every CodeHook on ONE line: tests/test_game_patches_config.py reads this file with a regex on {"Gow3Patch...", 0x...
  static const std::vector<Patch> t = {
      // show_fps: "Show FPS" by Sowa_95 (frame rate and frame time flags)
      {"show_fps", "Sowa_95", {{0x834290D8, {0x00}, {0x01}}, {0x834290EC, {0x00}, {0x01}}}, {}},
      // disable_motion_blur: "Disable Motion Blur" by boma (two lwz of the settings -> li 0). This game has no
      // uber post hash in its profile, so the option goes through these hooks.
      {"disable_motion_blur", "boma", {}, {
          {"Gow3PatchMotionBlur1", 0x82430640, 0x816A0038},
          {"Gow3PatchMotionBlur2", 0x827A2BCC, 0x816A0080},
      }},
      // disable_lens_flares: "Disable Lens Flares" by illusion (lwz r10,0x60(r11) -> li r10,0)
      {"disable_lens_flares", "illusion", {}, {
          {"Gow3PatchLensFlares", 0x82861FD0, 0x814B0060},
      }},
      // disable_bloom: "Disable Bloom" by boma (lwz r10,0x48(r11) -> li r10,0)
      {"disable_bloom", "boma", {}, {
          {"Gow3PatchBloom", 0x82593DC0, 0x814B0048},
      }},
      // frame_rate (internal): "Unlock FPS" by illusion, boma; its byte write is the low half of li r11,2
      // (present interval), here li r11,<interval>
      {kFrameRateIntervalKey, "illusion, boma", {}, {
          {"Gow3PatchPresentInterval", 0x82AF66BC, 0x39600002},
      }, true},
      // frame_rate (internal): "Unlock FPS" by illusion, boma (lfs f1,0x4C8(r3) skipped, tick rate without limit)
      {kFrameRateUnlockKey, "illusion, boma", {}, {
          {"Gow3PatchUnlockFps", 0x8253D9B0, 0xC02304C8},
      }, true},
      // coalesced.no_hash_check (internal, from [engine]): port of GoW2's "Disable Coalesced Hash Check" by
      // Sowa_95, boma (no GoW3 entry upstream). Same instruction shapes found in this image: the wrapper of
      // XexGetModuleSection called only with "HashSec" (li r3,1 -> li r3,0) and the handler that logs
      // "I/O failure operating on '%s'" and exits (mflr r12 -> blr).
      {kCoalescedNoHashCheckKey, "Sowa_95, boma", {}, {
          {"Gow3PatchCoalescedHash1", 0x82CE5D1C, 0x38600001},
          {"Gow3PatchCoalescedHash2", 0x8295B4D0, 0x7D8802A6},
      }, true},
      // aspect.21_9 (internal, from [video] aspect): "21:9 Aspect Ratio Ultrawide Patch" by boma. Xenia's code
      // cave sits on live code and here becomes a C++ hook; the ratio is 1280/550 (the patch rounds it to 2.3272).
      {kAspect21x9Key, "boma", {{0x820CDE2C, {0x3F, 0xE3, 0x8E, 0x4C}, {0x40, 0x14, 0xF2, 0x09}}}, {
          {"Gow3PatchNoConstrainAspect", 0x8261BE60, 0x419A0090},
          {"Gow3PatchUwFov", 0x8261BEF0, 0xC01B0630},
          {"Gow3PatchSceneHeight", 0x82960C78, 0x396002D0},
          {"Gow3PatchUwBufferHeight", 0x827C583C, 0x83C9242C},
          {"Gow3PatchUwUiScaleMode", 0x82BC9D84, 0x90830088},
      }, true},
      // aspect.16_10 (internal): 1280x800 Vert+, our port of GoW2's 16:10 entry onto the 21:9 sites above (ratio
      // and ConstrainAspectRatio by boma). The width is ratio x 720.0 clamped to 1280: the 720.0 becomes 800.0.
      {kAspect16x10Key, "boma", {{0x820CDE2C, {0x3F, 0xE3, 0x8E, 0x4C}, {0x3F, 0xCC, 0xCC, 0xCD}}}, {
          {"Gow3PatchNoConstrainAspect", 0x8261BE60, 0x419A0090},
          {"Gow3PatchWidthReference", 0x82960C6C, 0xC0140898},
          {"Gow3PatchSceneHeight", 0x82960C78, 0x396002D0},
      }, true},
  };
  return t;
}

}  // namespace game_patches

// [[midasm_hook]] hooks of games/gow3/gow3.toml: declared extern by the generated code, global C++ linkage.
// True = patch on: the register takes the patch's value and the original instruction is skipped; false =
// the original runs.
static bool ZeroIf(game_patches::PatchId id, PPCRegister& r) {
  if (!game_patches::Enabled(id)) return false;
  r.u64 = 0;
  return true;
}
bool Gow3PatchMotionBlur1(PPCRegister& r11) { return ZeroIf(game_patches::kDisableMotionBlur, r11); }
bool Gow3PatchMotionBlur2(PPCRegister& r11) { return ZeroIf(game_patches::kDisableMotionBlur, r11); }
bool Gow3PatchLensFlares(PPCRegister& r10) { return ZeroIf(game_patches::kDisableLensFlares, r10); }
bool Gow3PatchBloom(PPCRegister& r10) { return ZeroIf(game_patches::kDisableBloom, r10); }

bool Gow3PatchPresentInterval(PPCRegister& r11) {
  if (!game_patches::Enabled(game_patches::kFrameRateInterval)) return false;
  r11.u64 = game_patches::PresentInterval();
  return true;
}
bool Gow3PatchUnlockFps(PPCRegister&) { return game_patches::Enabled(game_patches::kFrameRateUnlock); }

bool Gow3PatchCoalescedHash1(PPCRegister& r3) { return ZeroIf(game_patches::kCoalescedNoHashCheck, r3); }
bool Gow3PatchCoalescedHash2() { return game_patches::Enabled(game_patches::kCoalescedNoHashCheck); }

static bool AnyAspect() {
  return game_patches::Enabled(game_patches::kAspect21x9) || game_patches::Enabled(game_patches::kAspect16x10);
}
bool Gow3PatchNoConstrainAspect() { return AnyAspect(); }
// 21:9, FOV: f0 = pi/360 and the caller does f2 = f31 * f0 (half angle in radians). Here f0 = atan(tan(f31*pi/360)
// * k) / f31 with k = (1280/550) / (16/9): Hor+ at every FOV, like GoW2's hook. FOV outside (0, 180): original.
bool Gow3PatchUwFov(PPCRegister& f0, PPCRegister& f31) {
  if (!game_patches::Enabled(game_patches::kAspect21x9)) return false;
  const double fov = f31.f64;
  if (!(fov > 0.0 && fov < 180.0)) return false;
  constexpr double kHorPlus = (1280.0 / 550.0) / (16.0 / 9.0);
  constexpr double kPi = 3.14159265358979323846;
  f0.f64 = double(float(std::atan(std::tan(fov * kPi / 360.0) * kHorPlus) / fov));
  return true;
}
// 16:10: reference height of the width 720.0 -> 800.0 (1.6 x 800 = 1280; the width is clamped to 900..1280).
bool Gow3PatchWidthReference(PPCRegister& f0) {
  if (!game_patches::Enabled(game_patches::kAspect16x10)) return false;
  f0.f64 = 800.0;
  return true;
}
// li r11,720 -> scene height, 550 at 21:9 and 800 at 16:10.
bool Gow3PatchSceneHeight(PPCRegister& r11) {
  if (game_patches::Enabled(game_patches::kAspect21x9)) {
    r11.u64 = 550;
  } else if (game_patches::Enabled(game_patches::kAspect16x10)) {
    r11.u64 = 800;
  } else {
    return false;
  }
  return true;
}
// 21:9: the scene buffer allocator reads 720 instead of the 550 scene height (as the original patch does).
bool Gow3PatchUwBufferHeight(PPCRegister& r30) {
  if (!game_patches::Enabled(game_patches::kAspect21x9)) return false;
  r30.u64 = 720;
  return true;
}
// 21:9: stw r4,0x88(r3) skipped (the original patch overwrites it with li r4,1, clobbered by the next addi r4).
bool Gow3PatchUwUiScaleMode() { return game_patches::Enabled(game_patches::kAspect21x9); }
