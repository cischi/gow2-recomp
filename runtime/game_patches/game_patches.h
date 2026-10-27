// Game patches (graphics): table, data patches and verification at startup.
// Code patches are hooks in the recompiled code (midasm_hook in config/gow2.toml) that read Enabled().
// Table rewritten as our own code from the xenia-canary/game-patches patches (no declared
// license); the authors are credited next to each entry.
#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "guest_memory.h"

namespace game_patches {

// Data write: written only if ALL the patch's `original` bytes are the expected ones.
struct DataWrite {
  uint32_t address;
  std::vector<uint8_t> original, patched;
};

// Hook on the recompiled code: `original` is the PPC word (big endian) expected at `address`.
struct CodeHook {
  const char* name;
  uint32_t address;
  uint32_t original;
};

struct Patch {
  const char* key;  // key of [patches] in gow2.toml (settings::kPatchKeys)
  const char* authors;
  std::vector<DataWrite> data;
  std::vector<CodeHook> hooks;
  bool internal = false;  // patch driven by [game] frame_rate or by [engine], not by [patches]: outside settings::kPatchKeys
};

// Keys of the internal patches (passed to Apply like the others).
inline constexpr const char* kFrameRateIntervalKey = "frame_rate.interval";
inline constexpr const char* kFrameRateUnlockKey = "frame_rate.unlock";
inline constexpr const char* kCoalescedNoHashCheckKey = "coalesced.no_hash_check";
inline constexpr const char* kAspect21x9Key = "aspect.21_9";
inline constexpr const char* kAspect16x10Key = "aspect.16_10";

// Index of the patch in All(), in table order.
enum PatchId : int {
  kShowFps, kDisableMotionBlur, kDisableLensFlares, kDisableBloom, kFrameRateInterval, kFrameRateUnlock, kCoalescedNoHashCheck, kAspect21x9, kAspect16x10, kPatchCount
};

// Present interval read by the hook (atomic; default 2 = the original).
void SetPresentInterval(uint32_t interval);

// Turns off a patch's flag (fallback to 30 fps) without a second Apply, which would find the bytes already written.
void Disable(int id);

std::span<const Patch> All();

// Verifies the patches requested in `enabled` (by key) on the image loaded in `mem`, writes the
// data and turns the flags on. Returns the log lines.
std::vector<std::string> Apply(GuestMemory& mem, const std::set<std::string>& enabled);

// Read by the hooks (atomic): true only if the patch was requested and passed verification.
bool Enabled(int id);

// Result of Apply's verification (also for patches not requested); internal ones only if requested.
bool Verified(int id);

// Turns a verified [patches] patch on/off hot (data: writes patched/original). False if internal or
// not verified; true also if the state was already that one (no log). Safe from a non-game thread.
bool SetEnabled(GuestMemory& mem, int id, bool on);

}  // namespace game_patches
