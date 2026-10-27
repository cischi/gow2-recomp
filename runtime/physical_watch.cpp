// Watching of guest physical pages (see physical_watch.h). Trigger
// follows Xenia's PhysicalHeap::TriggerCallbacks (src/xenia/memory.cc @
// 95a5c3ee250f80c3b9d139658649d9ffb6db3eec, BSD-3-Clause), Watch
// PhysicalHeap::EnableAccessCallbacks and OnWriteFault
// MMIOHandler::ExceptionCallback + Memory::AccessViolationCallback, with a single
// bit per physical page for the three views (heaps 0xA/0xC/0xE) and without the
// guest protection (here only logical, always read/write on the host).
#include "physical_watch.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "guest_memory.h"
#include "platform/platform.h"

namespace physwatch {

namespace {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kPhysicalSize = uint32_t(GuestMemory::kPhysicalSize);
constexpr uint32_t kPageCount = kPhysicalSize / kPage;
constexpr int kViews = 3;  // 0xA0000000, 0xC0000000, 0xE0000000
// Xenia TriggerCallbacks: maximum excess to stop watching beyond the requested
// pages (at most the 4 MiB block that contains them).
constexpr uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;

struct Entry {
  Callback callback;
  void* ctx;
};

// Everything under GlobalLock, except g_base/g_mem (atomic: read by the exception
// handler before taking the lock, and by IsAttached).
uint64_t g_watched[kPageCount / 64];
std::atomic<uint8_t*> g_base{nullptr};
std::atomic<const GuestMemory*> g_mem{nullptr};

std::vector<Entry*>& Callbacks() {
  static std::vector<Entry*> callbacks;
  return callbacks;
}

bool Bit(uint32_t page) { return (g_watched[page >> 6] >> (page & 63)) & 1; }
void SetBit(uint32_t page, bool on) {
  const uint64_t mask = uint64_t(1) << (page & 63);
  if (on) g_watched[page >> 6] |= mask;
  else g_watched[page >> 6] &= ~mask;
}

// Protection of pages [first, first+count) in all three views (protection is
// per view: the same physical page has to be changed three times).
void ProtectViews(uint8_t* base, uint32_t first, uint32_t count, bool read_only) {
  for (int v = 0; v < kViews; ++v) {
    uint8_t* p = base + GuestMemory::kPhysicalViewBegin + uint64_t(v) * GuestMemory::kPhysicalSize +
                 uint64_t(first) * kPage;
    if (read_only) plat::ProtectReadOnly(p, uint64_t(count) * kPage);
    else plat::ProtectReadWrite(p, uint64_t(count) * kPage);
  }
}

// Reopens and stops watching the watched pages of [first, last], in
// contiguous runs.
void UnwatchPages(uint8_t* base, uint32_t first, uint32_t last) {
  uint32_t run = UINT32_MAX;
  for (uint32_t i = first; i <= last + 1; ++i) {
    const bool watched = i <= last && Bit(i);
    if (watched) {
      SetBit(i, false);
      if (run == UINT32_MAX) run = i;
    } else if (run != UINT32_MAX) {
      if (base) ProtectViews(base, run, i - run, false);
      run = UINT32_MAX;
    }
  }
}

// Pages [first, last] covering [phys, phys+length) truncated to 512 MiB;
// false if the range is empty.
bool PageSpan(uint32_t phys, uint32_t length, uint32_t& first, uint32_t& last) {
  if (length == 0 || phys >= kPhysicalSize) return false;
  length = std::min(length, kPhysicalSize - phys);
  first = phys / kPage;
  last = (phys + length - 1) / kPage;
  return true;
}

}  // namespace

std::recursive_mutex& GlobalLock() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

void Init(GuestMemory& mem) {
  {
    std::lock_guard lock(GlobalLock());
    // A previous arena still alive becomes fully writable again.
    UnwatchPages(g_base.load(), 0, kPageCount - 1);
    g_base.store(mem.base);
    g_mem.store(&mem);
  }
  plat::InstallWriteFaultHandler(&OnWriteFault);
}

void Detach(const GuestMemory& mem) {
  std::lock_guard lock(GlobalLock());
  if (g_mem.load() != &mem) return;
  // The arena is about to be unmapped: forgetting the watched pages is enough.
  std::fill(std::begin(g_watched), std::end(g_watched), 0);
  g_base.store(nullptr);
  g_mem.store(nullptr);
}

bool IsAttached(const GuestMemory& mem) { return g_mem.load() == &mem; }

void* Register(Callback callback, void* ctx) {
  auto* entry = new Entry{callback, ctx};
  std::lock_guard lock(GlobalLock());
  Callbacks().push_back(entry);
  return entry;
}

void Unregister(void* handle) {
  if (!handle) return;
  auto* entry = static_cast<Entry*>(handle);
  // Under the same lock as Trigger: no Trigger is using the entry
  // while it is removed and deleted.
  std::lock_guard lock(GlobalLock());
  auto& callbacks = Callbacks();
  auto it = std::find(callbacks.begin(), callbacks.end(), entry);
  if (it == callbacks.end()) return;
  callbacks.erase(it);
  delete entry;
}

void Watch(uint32_t phys, uint32_t length) {
  uint32_t first = 0, last = 0;
  if (!PageSpan(phys, length, first, last)) return;
  std::lock_guard lock(GlobalLock());
  uint8_t* base = g_base.load();
  if (!base) return;
  uint32_t run = UINT32_MAX;
  for (uint32_t i = first; i <= last + 1; ++i) {
    const bool protect = i <= last && !Bit(i);
    if (protect) {
      SetBit(i, true);
      if (run == UINT32_MAX) run = i;
    } else if (run != UINT32_MAX) {
      ProtectViews(base, run, i - run, true);
      run = UINT32_MAX;
    }
  }
}

bool Trigger(uint32_t phys, uint32_t length, bool unwatch_exact) {
  uint32_t first = 0, last = 0;
  if (!PageSpan(phys, length, first, last)) return false;
  std::lock_guard lock(GlobalLock());
  bool any_watched = false;
  for (uint32_t i = first; i <= last && !any_watched; ++i) any_watched = Bit(i);
  if (!any_watched) return false;

  const uint32_t start = first * kPage;
  const uint32_t span = (last + 1) * kPage - start;
  uint32_t unwatch_first = 0, unwatch_last = UINT32_MAX;
  // By index: a callback that removes itself (recursive lock) does not
  // invalidate the iteration.
  auto& callbacks = Callbacks();
  for (size_t i = 0; i < callbacks.size(); ++i) {
    const Entry entry = *callbacks[i];
    const auto range = entry.callback(entry.ctx, start, span, unwatch_exact);
    if (!unwatch_exact) {
      unwatch_first = std::max(unwatch_first, range.first);
      const uint64_t range_last = uint64_t(range.first) + std::max(range.second, uint32_t(1)) - 1;
      unwatch_last = std::min<uint64_t>(unwatch_last, std::min<uint64_t>(range_last, UINT32_MAX));
    }
  }
  if (!unwatch_exact) {
    // Always at least the requested pages, at most their 4 MiB block.
    const uint32_t span_last = start + span - 1;
    unwatch_first = std::min(unwatch_first, start);
    unwatch_last = std::max(unwatch_last, span_last);
    unwatch_first = std::max(unwatch_first, start & ~(kMaxUnwatchExcess - 1));
    unwatch_last = std::min(unwatch_last, span_last | (kMaxUnwatchExcess - 1));
    unwatch_last = std::min(unwatch_last, kPhysicalSize - 1);
    first = unwatch_first / kPage;
    last = unwatch_last / kPage;
  }
  UnwatchPages(g_base.load(), first, last);
  return true;
}

bool OnWriteFault(uintptr_t host_address) {
  // Outside the physical views: no lock, the exception carries on.
  uint8_t* base = g_base.load();
  if (!base) return false;
  const uintptr_t views_begin = uintptr_t(base) + GuestMemory::kPhysicalViewBegin;
  const uintptr_t views_end = uintptr_t(base) + GuestMemory::kSize;
  if (host_address < views_begin || host_address >= views_end) return false;
  const uint32_t phys = uint32_t((host_address - views_begin) % GuestMemory::kPhysicalSize);
  std::lock_guard lock(GlobalLock());
  if (g_base.load() != base) return false;  // arena detached in the meantime
  if (Bit(phys / kPage)) {
    // Like Xenia: length 1, Trigger rounds to the page.
    Trigger(phys, 1, false);
    return true;
  }
  // Not watched: another thread may have just reopened it (re-execute),
  // otherwise it is a real access violation.
  return plat::IsWritable(reinterpret_cast<const void*>(host_address));
}

bool IsWatched(uint32_t phys) {
  if (phys >= kPhysicalSize) return false;
  std::lock_guard lock(GlobalLock());
  return Bit(phys / kPage);
}

}  // namespace physwatch
