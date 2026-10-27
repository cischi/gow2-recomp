#include "crash_map.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

namespace {

struct Entry {
  uintptr_t host;
  uint32_t guest;
};

// Sorted by host. It is read by the crash handler: no allocations there.
std::vector<Entry>& Map() {
  static std::vector<Entry> map;
  return map;
}
uintptr_t g_host_end = UINTPTR_MAX;

// Guard table: an entry is free when begin == 0. The write of end precedes the
// one of begin, so a reader seeing begin != 0 sees a valid end.
struct Guard {
  std::atomic<uintptr_t> begin{0}, end{0};
};
Guard g_guards[kMaxStackGuards];

void FormatCrashLineImpl(char* buf, size_t size, const plat::CrashInfo& info, bool in_guard) {
  if (!buf || size == 0) return;
  const uint32_t guest = GuestFunctionForHostPc(info.host_pc);
  char func[24] = "?";
  if (guest) std::snprintf(func, sizeof(func), "0x%X", guest);
  int n = std::snprintf(buf, size, "crash host=0x%llX guest_func=%s code=0x%X",
                        static_cast<unsigned long long>(info.host_pc), func, info.code);
  // append only if there is still room (snprintf truncates and always NUL-terminates)
  auto append = [&](const char* text) {
    if (n < 0 || size_t(n) >= size) return;
    const int m = std::snprintf(buf + n, size - size_t(n), "%s", text);
    n = m < 0 ? -1 : n + m;
  };
  if (info.has_data_address) {
    char addr[32];
    std::snprintf(addr, sizeof(addr), " addr=0x%llX", static_cast<unsigned long long>(info.data_address));
    append(addr);
    if (in_guard) append(" stack overflow");
  }
}

}  // namespace

void CrashMapInitFrom(std::span<const std::pair<uint32_t, uintptr_t>> guest_to_host, uintptr_t host_end) {
  std::vector<Entry> map;
  map.reserve(guest_to_host.size());
  for (const auto& [guest, host] : guest_to_host) map.push_back({host, guest});
  std::sort(map.begin(), map.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
  Map() = std::move(map);
  g_host_end = host_end;
}

uint32_t GuestFunctionForHostPc(uintptr_t pc) {
  const auto& map = Map();
  if (pc >= g_host_end) return 0;
  auto it = std::upper_bound(map.begin(), map.end(), pc,
                             [](uintptr_t value, const Entry& e) { return value < e.host; });
  if (it == map.begin()) return 0;
  return std::prev(it)->guest;
}

void FormatCrashLine(char* buf, size_t size, const plat::CrashInfo& info, uintptr_t guard_begin,
                     uintptr_t guard_end) {
  FormatCrashLineImpl(buf, size, info,
                      info.has_data_address && info.data_address >= guard_begin && info.data_address < guard_end);
}

void FormatCrashLine(char* buf, size_t size, const plat::CrashInfo& info) {
  FormatCrashLineImpl(buf, size, info, info.has_data_address && InStackGuard(info.data_address));
}

bool RegisterStackGuard(uintptr_t begin, uintptr_t end) {
  if (begin == 0 || end <= begin) return false;
  for (Guard& g : g_guards) {
    uintptr_t expected = 0;
    // reserve the entry with a provisional value (1 is never a valid guard:
    // InStackGuard discards it because end is still 0 or stale)
    if (!g.begin.compare_exchange_strong(expected, 1)) continue;
    g.end.store(end);
    g.begin.store(begin);
    return true;
  }
  return false;
}

void UnregisterStackGuard(uintptr_t begin) {
  if (begin <= 1) return;
  for (Guard& g : g_guards) {
    uintptr_t expected = begin;
    if (g.begin.compare_exchange_strong(expected, 0)) return;
  }
}

bool InStackGuard(uintptr_t address) {
  for (const Guard& g : g_guards) {
    const uintptr_t b = g.begin.load();
    if (b <= 1) continue;
    if (address >= b && address < g.end.load()) return true;
  }
  return false;
}
