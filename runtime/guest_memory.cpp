#include "guest_memory.h"

#include <algorithm>
#include <cstring>

#include "guest_address.h"
#include "physical_watch.h"
#include "platform/platform.h"

namespace {

constexpr uint64_t kPage = 0x1000;
constexpr int kPhysicalViews = 3;  // 0xA0000000, 0xC0000000, 0xE0000000

// End (exclusive) of the guest addresses with a physical byte: the last guest
// page, 0xFFFFF000, is physical 0x20000000 because of the 0xE view shift.
constexpr uint64_t kGuestPhysicalEnd = GuestMemory::kSize - gow2::kViewEOffset;

// 4 KB pages covering [guest, guest+size); false if they go beyond 4 GB or
// touch the last guest page (checked before any effect).
bool PageRange(uint32_t guest, uint32_t size, uint64_t& begin, uint64_t& length) {
  begin = guest & ~(kPage - 1);
  const uint64_t end = (uint64_t(guest) + std::max<uint32_t>(size, 1) + kPage - 1) & ~(kPage - 1);
  if (end > kGuestPhysicalEnd) return false;
  length = end - begin;
  return true;
}

uint64_t ViewBase(int view) { return GuestMemory::kPhysicalViewBegin + uint64_t(view) * GuestMemory::kPhysicalSize; }

// Applies `low(begin, length)` to the part of [begin, begin+length) below the
// physical views and `phys(physical, length)` to the pieces inside the views
// (split at the boundary of each view; the physical address is gow2::PhysicalAddressOf,
// so in the 0xE view shifted by 0x1000). A piece beyond the 512 MiB physical
// makes the operation fail. false at the first failure.
template <typename Low, typename Phys>
bool SplitRange(uint64_t begin, uint64_t length, Low low, Phys phys) {
  const uint64_t end = begin + length;
  if (begin < GuestMemory::kPhysicalViewBegin) {
    const uint64_t low_end = std::min(end, GuestMemory::kPhysicalViewBegin);
    if (!low(begin, low_end - begin)) return false;
    begin = low_end;
  }
  while (begin < end) {
    const uint64_t offset = (begin - GuestMemory::kPhysicalViewBegin) % GuestMemory::kPhysicalSize;
    const uint64_t chunk = std::min(end - begin, GuestMemory::kPhysicalSize - offset);
    const uint64_t physical = gow2::PhysicalAddressOf(uint32_t(begin));
    if (physical + chunk > GuestMemory::kPhysicalSize) return false;
    if (!phys(physical, chunk)) return false;
    begin += chunk;
  }
  return true;
}

}  // namespace

GuestMemory::~GuestMemory() {
  if (!base) return;
  physwatch::Detach(*this);
  if (physical_section) {
    for (int v = 0; v < kPhysicalViews; ++v) plat::UnmapView(base + ViewBase(v));
    plat::CloseSharedMemory(physical_section);
  }
  plat::Release(base + kSize, kTailReserve);
  plat::Release(base, kPhysicalViewBegin);
}

bool GuestMemory::Init() {
  if (base) return true;
  // Placeholder of 4 GB + kTailReserve split into [0, 0xA0000000) (normal
  // reserve), three 512 MiB views of the same section (Xenia memory.cc,
  // map_info) and the tail [4 GB, 4 GB + kTailReserve) (normal reserve never
  // committed: the last guest page of 0xE, see guest_memory.h).
  physical_section = plat::CreateSharedMemory(kPhysicalSize);
  auto* p = static_cast<uint8_t*>(plat::ReservePlaceholder(kPreferredBase, kSize + kTailReserve));
  bool ok = physical_section && p;
  // Splitting an inner piece leaves what follows it as a placeholder of its own:
  // after the last view the tail stays separate.
  for (int v = 0; ok && v < kPhysicalViews; ++v) ok = plat::SplitPlaceholder(p + ViewBase(v), kPhysicalSize);
  ok = ok && plat::PlaceholderToReserve(p, kPhysicalViewBegin);
  ok = ok && plat::PlaceholderToReserve(p + kSize, kTailReserve);
  bool mapped[kPhysicalViews] = {};
  for (int v = 0; ok && v < kPhysicalViews; ++v)
    ok = mapped[v] = plat::MapViewAt(physical_section, p + ViewBase(v), 0, kPhysicalSize);
  // The whole section committed (like Xenia's SEC_COMMIT): visible, zeroed,
  // from all three views; the kernel's budget stays logical.
  ok = ok && plat::Commit(p + kPhysicalViewBegin, kPhysicalSize);
  if (ok) {
    base = p;
    return true;
  }
  // Failure: views unmapped, then every piece still reserved is released
  // (Release on an address that is not the start of a piece does nothing).
  if (p) {
    for (int v = 0; v < kPhysicalViews; ++v) {
      if (mapped[v]) plat::UnmapView(p + ViewBase(v));
      else plat::Release(p + ViewBase(v), kPhysicalSize);
    }
    plat::Release(p + kSize, kTailReserve);
    plat::Release(p, kPhysicalViewBegin);
  }
  plat::CloseSharedMemory(physical_section);
  physical_section = nullptr;
  return false;
}

bool GuestMemory::CommitPhysical(uint64_t phys, uint64_t length) {
  // The section is always committed: "commit" here is the handover of a
  // physical allocation. First the GPU is invalidated (watched pages are
  // reopened), then zeroed from one view: all three see it.
  if (physwatch::IsAttached(*this)) physwatch::Trigger(uint32_t(phys), uint32_t(length), true);
  std::memset(base + kPhysicalViewBegin + phys, 0, length);
  return true;
}

bool GuestMemory::DecommitPhysical(uint64_t phys, uint64_t length) {
  // Physical free: the GPU must no longer trust its copies (Xenia
  // PhysicalHeap::Decommit/Release); the pages stay committed and
  // readable, with their previous content, until a new allocation
  // zeroes them.
  if (physwatch::IsAttached(*this)) physwatch::Trigger(uint32_t(phys), uint32_t(length), true);
  return true;
}

bool GuestMemory::Commit(uint32_t guest, uint32_t size) {
  uint64_t begin = 0, length = 0;
  if (!base || !PageRange(guest, size, begin, length)) return false;
  return SplitRange(
      begin, length, [&](uint64_t b, uint64_t n) { return plat::Commit(base + b, n); },
      [&](uint64_t phys, uint64_t n) { return CommitPhysical(phys, n); });
}

bool GuestMemory::Decommit(uint32_t guest, uint32_t size) {
  uint64_t begin = 0, length = 0;
  if (!base || !PageRange(guest, size, begin, length)) return false;
  return SplitRange(
      begin, length, [&](uint64_t b, uint64_t n) { return plat::Decommit(base + b, n); },
      [&](uint64_t phys, uint64_t n) { return DecommitPhysical(phys, n); });
}

uint32_t GuestMemory::Alloc(uint32_t size, uint32_t align) {
  if (align == 0) align = 1;
  if (align & (align - 1)) return 0;
  const uint64_t start = (next_ + align - 1) & ~uint64_t(align - 1);
  const uint64_t end = start + std::max<uint32_t>(size, 1);  // addresses always distinct
  if (end > kAllocLimit) return 0;
  if (!Commit(static_cast<uint32_t>(start), static_cast<uint32_t>(end - start))) return 0;
  next_ = end;
  return static_cast<uint32_t>(start);
}

void GuestMemory::SetHeapStart(uint32_t guest_address) {
  // never below page 0: Alloc uses 0 as the failure value
  next_ = std::max(guest_address, kDefaultHeapStart);
}
