#include "game_patches/game_patches.h"
#include "game_profile.h"
#include "log.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace game_patches {
namespace {

std::array<std::atomic<bool>, kPatchCount> g_enabled{};
std::array<std::atomic<bool>, kPatchCount> g_verified{};  // result of Apply's verification, also for patches not requested

std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

std::atomic<uint32_t> g_present_interval{2};

}  // namespace

std::span<const Patch> All() { return Table(); }

bool Available(int id) { return id >= 0 && id < kPatchCount && All()[size_t(id)].available; }

uint32_t PresentInterval() { return g_present_interval.load(std::memory_order_relaxed); }

void SetPresentInterval(uint32_t interval) { g_present_interval.store(interval, std::memory_order_relaxed); }

void Disable(int id) {
  if (id >= 0 && id < kPatchCount) g_enabled[size_t(id)].store(false);
}

bool Enabled(int id) { return id >= 0 && id < kPatchCount && g_enabled[size_t(id)].load(std::memory_order_relaxed); }

namespace {
// True if the expected bytes/instructions are there; otherwise a log line in `log` (tag included) and false.
bool DataMatches(GuestMemory& mem, const Patch& p, const std::string& tag, std::vector<std::string>& log) {
  for (const DataWrite& w : p.data) {
    if (std::memcmp(mem.base + w.address, w.original.data(), w.original.size()) != 0) {
      log.push_back(tag + "unexpected bytes at " + Hex(w.address) + ", skipped");
      return false;
    }
  }
  return true;
}
}  // namespace

std::vector<std::string> Apply(GuestMemory& mem, const std::set<std::string>& enabled) {
  std::vector<std::string> log;
  for (auto& f : g_enabled) f.store(false);
  for (auto& v : g_verified) v.store(false);
  const auto all = All();
  // First everything is verified (also patches not requested, to turn them on hot), then written.
  for (size_t id = 0; id < all.size(); ++id) {
    const Patch& p = all[id];
    if (!p.available) {  // not in this game: never verified
      if (enabled.count(p.key)) log.push_back(std::string("[patch] ") + p.key + ": not available for this game");
      continue;
    }
    if (p.internal && !enabled.count(p.key)) continue;
    // Motion blur works through shader constants in a game with a post capture (main.cpp removes it from
    // the set): no byte verification. Without it its hooks are verified like the others.
    if (id == kDisableMotionBlur && game_profile::Get().post.motion_ps && !enabled.count(p.key)) continue;
    const std::string tag = std::string("[patch] ") + p.key + ": ";
    bool ok = DataMatches(mem, p, tag, log);
    for (size_t i = 0; ok && i < p.hooks.size(); ++i) {
      const CodeHook& h = p.hooks[i];
      uint32_t raw;
      std::memcpy(&raw, mem.base + h.address, 4);
      if (__builtin_bswap32(raw) != h.original) {
        log.push_back(tag + "unexpected instruction at " + Hex(h.address) + " (" + h.name + "), skipped");
        ok = false;
      }
    }
    g_verified[id].store(ok);
  }
  for (size_t id = 0; id < all.size(); ++id) {
    const Patch& p = all[id];
    if (!enabled.count(p.key) || !g_verified[id].load()) continue;
    const std::string tag = std::string("[patch] ") + p.key + ": ";
    // Entries that touch the same bytes (21:9 and 16:10): the second finds the first already written.
    if (!DataMatches(mem, p, tag, log)) continue;
    for (const DataWrite& w : p.data) std::memcpy(mem.base + w.address, w.patched.data(), w.patched.size());
    g_enabled[id].store(true);
    log.push_back(tag + "turned on");
  }
  return log;
}

bool Verified(int id) { return id >= 0 && id < kPatchCount && g_verified[size_t(id)].load(std::memory_order_relaxed); }

bool SetEnabled(GuestMemory& mem, int id, bool on) {
  if (!Verified(id) || All()[size_t(id)].internal) return false;
  if (g_enabled[size_t(id)].load() == on) return true;
  const Patch& p = All()[size_t(id)];
  for (const DataWrite& w : p.data) {
    const auto& src = on ? w.patched : w.original;
    std::memcpy(mem.base + w.address, src.data(), src.size());
  }
  g_enabled[size_t(id)].store(on);
  LogLine(std::string("[patch] ") + p.key + (on ? ": turned on hot" : ": turned off hot"));
  return true;
}

}  // namespace game_patches
