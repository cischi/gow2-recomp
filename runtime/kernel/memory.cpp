// Guest memory of the HLE kernel: allocators (see memory.h) and the
// Nt*VirtualMemory, Mm*, Rtl*Heap exports.
//
// Each export cites the Xenia function (https://github.com/xenia-project/xenia
// @ 95a5c3ee, BSD), src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc. Deviations
// are noted case by case.
//
// Rtl{Allocate,Free,Size,ReAllocate}Heap (process heap on o1heap, flag
// HEAP_ZERO_MEMORY 0x8) follow the semantics of Unleashed Recomp
// (https://github.com/hedge-dev/UnleashedRecomp), file
// UnleashedRecomp/kernel/heap.cpp, commit cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c,
// license GPL-3.0 (see LICENSE and NOTICE). Rewritten: block sizes
// are kept in a map instead of reading o1heap's internal header, and
// RtlReAllocateHeap uses o1heapReallocate (3.x) instead of alloc+copy+free.
#include "kernel/memory.h"

#include <o1heap.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "guest_address.h"
#include "guest_memory.h"
#include "kernel/registry.h"
#include "kernel/rtl_exports.h"
#include "log.h"

namespace kernel {

namespace {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kGranularity = 0x10000;  // base of reservations, like Windows/Xenia

std::atomic<uint64_t> g_in_use{0};

// Charges n bytes of the budget; false (no side effects) if it would be exceeded.
bool Charge(uint64_t n) {
  uint64_t cur = g_in_use.load();
  do {
    if (n > kBudget || cur > kBudget - n) return false;
  } while (!g_in_use.compare_exchange_weak(cur, cur + n));
  return true;
}
void Uncharge(uint64_t n) { g_in_use.fetch_sub(n); }

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::unique_ptr<MemorySystem> g_memory;

}  // namespace

uint64_t BytesInUse() { return g_in_use.load(); }
uint32_t CanonicalPhysicalAddress(uint32_t guest) {
  // From 0xE with the 4 KB offset: the last page (physical 0x20000000) becomes
  // 0xC0000000-0xC0000FFF, outside the physical heap and any virtual heap.
  return guest >= 0xC0000000u ? kPhysicalHeapBegin + gow2::PhysicalAddressOf(guest) : guest;
}

namespace {

// Canonical address (0xA view) brought into view `view` (0 = unchanged,
// 0xC0000000 or 0xE0000000): inverse of CanonicalPhysicalAddress.
uint32_t InView(uint32_t view, uint32_t canonical) {
  if (view == 0) return canonical;
  const uint32_t phys = canonical - kPhysicalHeapBegin;
  return view == gow2::kViewEBegin ? view + phys - gow2::kViewEOffset : view | phys;
}

}  // namespace

// ---- virtual heap ---------------------------------------------------------

VirtualAllocator::VirtualAllocator(GuestMemory& mem, uint32_t begin, uint32_t end, uint32_t granularity)
    : mem_(mem),
      begin_(uint32_t(AlignUp(begin, std::max(granularity, kPage)))),
      end_(std::max(begin_, end & ~(kPage - 1))),
      granularity_(std::max(granularity, kPage)),
      pages_((end_ - begin_) / kPage) {}

VirtualAllocator::~VirtualAllocator() {
  // The committed pages go back to the arena (reservation) and the budget is freed.
  for (const auto& [base, alloc] : allocs_) DecommitRange(base, alloc.size, alloc.charged);
}

uint32_t VirtualAllocator::FindFree(uint64_t low, uint64_t high, uint32_t size, uint32_t align,
                                    bool top_down) const {
  low = std::max<uint64_t>(low, begin_);
  high = std::min<uint64_t>(high, end_);
  if (size == 0 || low >= high || size > high - low) return 0;
  if (!top_down) {
    for (uint64_t b = AlignUp(low, align); b + size <= high;) {
      size_t busy = SIZE_MAX;
      for (size_t i = Index(uint32_t(b)), n = Index(uint32_t(b + size - 1)); i <= n; ++i)
        if (pages_[i].alloc_base) {
          busy = i;
          break;
        }
      if (busy == SIZE_MAX) return uint32_t(b);
      b = AlignUp(begin_ + uint64_t(busy + 1) * kPage, align);  // past the busy page
    }
    return 0;
  }
  uint64_t b = (high - size) & ~uint64_t(align - 1);
  while (b >= low) {
    size_t busy = SIZE_MAX;
    for (size_t i = Index(uint32_t(b + size - 1)) + 1, n = Index(uint32_t(b)); i-- > n;)
      if (pages_[i].alloc_base) {
        busy = i;
        break;
      }
    if (busy == SIZE_MAX) return uint32_t(b);
    const uint64_t busy_addr = begin_ + uint64_t(busy) * kPage;  // below the busy page
    if (busy_addr < low + size) return 0;
    b = (busy_addr - size) & ~uint64_t(align - 1);
  }
  return 0;
}

bool VirtualAllocator::CommitRange(uint32_t addr, uint32_t size, uint32_t protect, bool charged) {
  uint64_t fresh = 0;
  for (size_t i = Index(addr), n = Index(addr + size - 1); i <= n; ++i)
    if (!pages_[i].committed) fresh += kPage;
  if (charged && !Charge(fresh)) return false;
  // Commit on already committed pages is allowed and does not zero them: only the new
  // pages go to the arena, in contiguous ranges (in the physical views
  // GuestMemory::Commit always zeroes and invalidates the GPU). If one piece
  // fails, the earlier ones stay committed in the arena but not marked here.
  for (size_t i = Index(addr), n = Index(addr + size - 1), run = SIZE_MAX; i <= n + 1; ++i) {
    const bool is_fresh = i <= n && !pages_[i].committed;
    if (is_fresh && run == SIZE_MAX) run = i;
    if (is_fresh || run == SIZE_MAX) continue;
    if (!mem_.Commit(begin_ + uint32_t(run) * kPage, uint32_t(i - run) * kPage)) {
      if (charged) Uncharge(fresh);
      return false;
    }
    run = SIZE_MAX;
  }
  for (size_t i = Index(addr), n = Index(addr + size - 1); i <= n; ++i) {
    pages_[i].committed = true;
    pages_[i].protect = protect;
  }
  return true;
}

void VirtualAllocator::DecommitRange(uint32_t addr, uint32_t size, bool charged) {
  uint64_t freed = 0;
  for (size_t i = Index(addr), n = Index(addr + size - 1); i <= n; ++i) {
    if (pages_[i].committed) freed += kPage;
    pages_[i].committed = false;
    pages_[i].protect = 0;
  }
  // inaccessible again and zeroed at the next commit (physical: GPU invalidated, pages readable)
  if (freed) mem_.Decommit(addr, size);
  if (charged) Uncharge(freed);
}

uint32_t VirtualAllocator::AllocateLocked(uint32_t hint, uint32_t size, uint32_t alloc_type, uint32_t protect,
                                          bool charged, uint32_t* out_size, bool kernel) {
  if (size == 0 || !(alloc_type & (kMemReserve | kMemCommit))) return 0;
  const uint32_t page = (alloc_type & kMemLargePages) ? kGranularity : kPage;
  uint32_t base = 0;
  uint64_t length = 0;
  if (hint) {
    if (!Contains(hint)) return 0;
    const uint32_t owner = pages_[Index(hint)].alloc_base;
    if (owner) {
      // Inside an existing reservation: commit only, all in the same
      // allocation, in heap pages (granularity_: 64 KB for the
      // 0x40000000 heap) like Xenia NtAllocateVirtualMemory, which with an address
      // uses the heap's page_size. UE3 commits 0x3B140 bytes and uses the
      // whole 64 KB page. The commit does not leave the reservation.
      if (!(alloc_type & kMemCommit)) return 0;
      base = hint & ~(granularity_ - 1);
      if (base < owner) base = owner;
      const uint64_t owner_end = uint64_t(owner) + allocs_.at(owner).size;
      const uint64_t end = std::min(AlignUp(uint64_t(hint) + size, granularity_), owner_end);
      if (end > end_ || end < uint64_t(hint) + size) return 0;
      for (size_t i = Index(base), n = Index(uint32_t(end - 1)); i <= n; ++i)
        if (pages_[i].alloc_base != owner) return 0;
      length = end - base;
      if (!CommitRange(base, uint32_t(length), protect, allocs_.at(owner).charged)) return 0;
      if (out_size) *out_size = uint32_t(length);
      return base;
    }
    base = hint & ~(granularity_ - 1);
    const uint64_t end = AlignUp(uint64_t(hint) + size, page);
    if (end > end_) return 0;
    length = end - base;
    for (size_t i = Index(base), n = Index(uint32_t(end - 1)); i <= n; ++i)
      if (pages_[i].alloc_base) return 0;
  } else {
    length = AlignUp(size, page);
    if (length > end_ - begin_) return 0;
    base = FindFree(begin_, end_, uint32_t(length), granularity_, (alloc_type & kMemTopDown) != 0);
    if (!base) return 0;
  }
  return Reserve(base, length, alloc_type, protect, charged, out_size, kernel);
}

// Marks [base, base+length) (free pages) as a new allocation and, with
// kMemCommit, commits it; if the commit fails (budget) it leaves no trace.
uint32_t VirtualAllocator::Reserve(uint32_t base, uint64_t length, uint32_t alloc_type, uint32_t protect,
                                   bool charged, uint32_t* out_size, bool kernel) {
  for (size_t i = Index(base), n = Index(uint32_t(base + length - 1)); i <= n; ++i) pages_[i] = {base, 0, false};
  allocs_[base] = {uint32_t(length), protect, charged, kernel};
  if ((alloc_type & kMemCommit) && !CommitRange(base, uint32_t(length), protect, charged)) {
    for (size_t i = Index(base), n = Index(uint32_t(base + length - 1)); i <= n; ++i) pages_[i] = {};
    allocs_.erase(base);
    return 0;
  }
  if (out_size) *out_size = uint32_t(length);
  return base;
}

uint32_t VirtualAllocator::Allocate(uint32_t hint, uint32_t size, uint32_t alloc_type, uint32_t protect,
                                    uint32_t* out_size) {
  std::lock_guard lock(mutex_);
  return AllocateLocked(hint, size, alloc_type, protect, true, out_size);
}

uint32_t VirtualAllocator::AllocateArena(uint32_t size) {
  std::lock_guard lock(mutex_);
  return AllocateLocked(0, size, kMemReserve | kMemCommit | kMemTopDown, kPageReadWrite, false, nullptr, true);
}

bool VirtualAllocator::IsKernelOwned(uint32_t addr) const {
  std::lock_guard lock(mutex_);
  if (!Contains(addr)) return false;
  const uint32_t owner = pages_[Index(addr)].alloc_base;
  return owner && allocs_.at(owner).kernel;
}

uint32_t VirtualAllocator::AllocateRange(uint32_t low, uint32_t high, uint32_t size, uint32_t align,
                                         uint32_t protect, bool top_down) {
  if (size == 0 || (align & (align - 1))) return 0;
  align = std::max(align, kPage);
  const uint64_t length = AlignUp(size, kPage);
  std::lock_guard lock(mutex_);
  const uint32_t base = FindFree(low, uint64_t(high) + 1, uint32_t(std::min<uint64_t>(length, UINT32_MAX)), align,
                                 top_down);
  if (!base) return 0;
  return Reserve(base, length, kMemReserve | kMemCommit, protect, true, nullptr);
}

bool VirtualAllocator::Free(uint32_t addr, uint32_t size, uint32_t free_type, uint32_t* out_size) {
  std::lock_guard lock(mutex_);
  if (!Contains(addr)) return false;
  const uint32_t owner = pages_[Index(addr)].alloc_base;
  if (!owner) return false;
  const Allocation alloc = allocs_.at(owner);
  if (alloc.kernel) return false;  // arena of an internal allocator: only the destructor releases it
  const uint64_t alloc_end = uint64_t(owner) + alloc.size;
  if (free_type == kMemDecommit) {
    const uint32_t base = addr & ~(kPage - 1);
    const uint64_t end = size ? AlignUp(uint64_t(addr) + size, kPage) : alloc_end;
    if (end > alloc_end) return false;
    DecommitRange(base, uint32_t(end - base), alloc.charged);
    if (out_size) *out_size = uint32_t(end - base);
    return true;
  }
  if (free_type == kMemRelease) {
    if (addr != owner) return false;  // release only from the base (Xenia BaseHeap::Release)
    DecommitRange(owner, alloc.size, alloc.charged);
    for (size_t i = Index(owner), n = Index(uint32_t(alloc_end - 1)); i <= n; ++i) pages_[i] = {};
    allocs_.erase(owner);
    if (out_size) *out_size = alloc.size;
    return true;
  }
  return false;
}

bool VirtualAllocator::Query(uint32_t addr, RegionInfo& info) const {
  std::lock_guard lock(mutex_);
  if (!Contains(addr)) return false;
  const size_t first = Index(addr);
  const Page& p = pages_[first];
  size_t n = first;
  while (n < pages_.size() && pages_[n].alloc_base == p.alloc_base && pages_[n].committed == p.committed &&
         pages_[n].protect == p.protect)
    ++n;
  info = {};
  info.base_address = addr & ~(kPage - 1);
  info.region_size = uint32_t((n - first) * kPage);
  if (!p.alloc_base) {
    info.state = kMemFree;
    return true;
  }
  info.allocation_base = p.alloc_base;
  info.allocation_protect = allocs_.at(p.alloc_base).protect;
  info.state = p.committed ? kMemCommit : kMemReserve;
  info.protect = p.committed ? p.protect : 0;
  return true;
}

bool VirtualAllocator::Protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* old) {
  std::lock_guard lock(mutex_);
  if (size == 0 || !Contains(addr)) return false;
  const uint32_t base = addr & ~(kPage - 1);
  const uint64_t end = AlignUp(uint64_t(addr) + size, kPage);
  if (end > end_) return false;
  const uint32_t owner = pages_[Index(base)].alloc_base;
  for (size_t i = Index(base), n = Index(uint32_t(end - 1)); i <= n; ++i)
    if (!owner || pages_[i].alloc_base != owner || !pages_[i].committed) return false;
  if (old) *old = pages_[Index(base)].protect;
  for (size_t i = Index(base), n = Index(uint32_t(end - 1)); i <= n; ++i) pages_[i].protect = protect;
  return true;
}

uint32_t VirtualAllocator::QueryProtect(uint32_t addr) const {
  std::lock_guard lock(mutex_);
  if (!Contains(addr)) return 0;
  const Page& p = pages_[Index(addr)];
  return p.committed ? p.protect : 0;
}

uint32_t VirtualAllocator::AllocationSize(uint32_t addr) const {
  std::lock_guard lock(mutex_);
  if (!Contains(addr)) return 0;
  const uint32_t owner = pages_[Index(addr)].alloc_base;
  return owner ? allocs_.at(owner).size : 0;
}

// ---- physical heap ----------------------------------------------------------

namespace {
constexpr uint32_t kPhysicalHeapSize = uint32_t(kPhysicalHeapEnd - kPhysicalHeapBegin);
}

// Xenia PhysicalHeap/BaseHeap (src/xenia/memory.cc): pages reserved and
// committed per block, no initial commit. 4 KB granularity: small
// blocks do not each consume 64 KB of space.
PhysicalHeap::PhysicalHeap(GuestMemory& mem)
    : pages_(mem, kPhysicalHeapBegin, uint32_t(kPhysicalHeapEnd), kPage) {}

uint32_t PhysicalHeap::Alloc(uint32_t size, uint32_t align, uint32_t protect, uint32_t low, uint32_t high) {
  if (align == 0) align = kPage;
  // Like MmAllocatePhysicalMemoryEx in Xenia: from the top (top_down).
  return pages_.AllocateRange(low, high, std::max<uint32_t>(size, 1), align, protect, true);
}

void PhysicalHeap::Free(uint32_t addr) { pages_.Free(addr, 0, kMemRelease); }

bool PhysicalHeap::Free(uint32_t addr, uint32_t size, uint32_t free_type, uint32_t* out_size) {
  return pages_.Free(addr, size, free_type, out_size);
}

uint32_t PhysicalHeap::Size(uint32_t addr) const {
  RegionInfo info;
  if (!pages_.Query(addr, info) || info.allocation_base != addr) return 0;
  return pages_.AllocationSize(addr);
}

bool PhysicalHeap::Find(uint32_t addr, uint32_t& base, uint32_t& size, uint32_t& protect) const {
  RegionInfo info;
  if (!pages_.Query(addr, info) || info.state != kMemCommit) return false;
  base = info.allocation_base;
  size = pages_.AllocationSize(addr);
  protect = info.protect;
  return true;
}

bool PhysicalHeap::SetProtect(uint32_t addr, uint32_t protect) {
  uint32_t base = 0, size = 0, old = 0;
  if (!Find(addr, base, size, old)) return false;
  return pages_.Protect(base, size, protect);
}

// ---- process heap -----------------------------------------------------------

ProcessHeap::ProcessHeap(GuestMemory& mem, VirtualAllocator& virt, uint32_t arena_size) : mem_(mem) {
  const uint32_t arena = mem.base ? virt.AllocateArena(arena_size) : 0;
  if (arena) heap_ = o1heapInit(mem.base + arena, arena_size);
}

ProcessHeap::~ProcessHeap() {
  // The arena belongs to the virtual heap, which releases it in its destructor.
  uint64_t in_use = 0;
  for (const auto& [addr, size] : sizes_) in_use += size;
  Uncharge(in_use);
}

uint32_t ProcessHeap::Alloc(uint32_t size) {
  if (!heap_) return 0;
  std::lock_guard lock(mutex_);
  if (!Charge(size)) return 0;
  void* p = o1heapAllocate(heap_, std::max<size_t>(size, 1));
  if (!p) {
    Uncharge(size);
    return 0;
  }
  const uint32_t addr = uint32_t(static_cast<uint8_t*>(p) - mem_.base);
  sizes_[addr] = size;
  return addr;
}

void ProcessHeap::Free(uint32_t addr) {
  std::lock_guard lock(mutex_);
  auto it = sizes_.find(addr);
  if (it == sizes_.end()) return;  // foreign pointer: ignored instead of corrupting o1heap
  o1heapFree(heap_, mem_.base + addr);
  Uncharge(it->second);
  sizes_.erase(it);
}

uint32_t ProcessHeap::Size(uint32_t addr) const {
  std::lock_guard lock(mutex_);
  auto it = sizes_.find(addr);
  return it == sizes_.end() ? 0 : it->second;
}

uint32_t ProcessHeap::Realloc(uint32_t addr, uint32_t size) {
  if (!addr) return Alloc(size);
  if (!heap_) return 0;
  std::lock_guard lock(mutex_);
  auto it = sizes_.find(addr);
  if (it == sizes_.end()) return 0;
  const uint32_t old = it->second;
  if (size > old && !Charge(size - old)) return 0;
  void* p = o1heapReallocate(heap_, mem_.base + addr, std::max<size_t>(size, 1));
  if (!p) {
    if (size > old) Uncharge(size - old);
    return 0;  // the old block stays valid
  }
  if (size < old) Uncharge(old - size);
  sizes_.erase(it);
  const uint32_t moved = uint32_t(static_cast<uint8_t*>(p) - mem_.base);
  sizes_[moved] = size;
  return moved;
}

// ---- system used by the exports ---------------------------------------------

MemorySystem::MemorySystem(GuestMemory& mem)
    : virt(mem, kVirtualHeapBegin, kVirtualHeapEnd),
      virt4k(mem, kVirtual4kHeapBegin, kVirtual4kHeapEnd, 0x1000),
      phys(mem),
      heap(mem, virt) {}

namespace {
uint32_t SystemAlloc(uint32_t size) { return g_memory ? g_memory->heap.Alloc(size) : 0; }
void SystemFree(uint32_t addr) {
  if (g_memory) g_memory->heap.Free(addr);
}
}  // namespace

bool InitMemory(GuestMemory& mem) {
  ShutdownMemory();
  if (!mem.base) return false;
  auto system = std::make_unique<MemorySystem>(mem);
  if (!system->heap.Valid()) return false;
  g_memory = std::move(system);
  rtl::SetSystemHeap(&SystemAlloc, &SystemFree);
  return true;
}

void ShutdownMemory() {
  rtl::SetSystemHeap(nullptr, nullptr);
  g_memory.reset();
}

MemorySystem* Memory() { return g_memory.get(); }

}  // namespace kernel

// ---- exports --------------------------------------------------------------------

namespace {

using namespace kernel;

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusAccessDenied = 0xC0000022;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
constexpr uint32_t kStatusMemoryNotAllocated = 0xC00000A0;

constexpr uint32_t kPageExecuteAny = 0x10 | 0x20 | 0x40 | 0x80;  // X_PAGE_EXECUTE*
constexpr uint32_t kHeapZeroMemory = 0x8;

// Protection as reported by queries (Xenia ToXdkProtectFlags(FromXdkProtectFlags(p))).
uint32_t NormalizeProtect(uint32_t p) {
  uint32_t r = kPageNoAccess;
  if (p & (kPageReadOnly | kPageExecuteRead))
    r = kPageReadOnly;
  else if (p & (kPageReadWrite | kPageExecuteReadWrite))
    r = kPageReadWrite;
  return r | (p & (kPageNoCache | kPageWriteCombine));
}

// Xenia: NtAllocateVirtualMemory_entry. Like Xenia (LookupHeapByType) with
// X_MEM_LARGE_PAGES the allocation goes into the 64 KB heap (0x40000000), without
// it into the 4 KB one (virt4k, below 0x40000000); with a requested address
// the heap that contains it decides, and a commit inside a reservation is in heap
// pages (64 KB above 0x40000000). Deviations: X_MEM_RESET alone does
// nothing (Xenia: not implemented); the 512 MiB budget yields STATUS_NO_MEMORY.
uint32_t NtAllocateVirtualMemoryImpl(be<uint32_t>* base_ptr, be<uint32_t>* size_ptr, uint32_t alloc_type,
                                     uint32_t protect, uint32_t /*debug_memory*/) {
  if (!base_ptr || !size_ptr || !*size_ptr) return kStatusInvalidParameter;
  if (!(alloc_type & (kMemCommit | kMemReset | kMemReserve))) return kStatusInvalidParameter;
  if ((alloc_type & kMemReset) && (alloc_type & ~kMemReset)) return kStatusInvalidParameter;
  if (alloc_type & kMemReset) return kStatusSuccess;
  MemorySystem* m = Memory();
  if (!m) return kStatusNoMemory;
  const uint32_t hint = *base_ptr;
  VirtualAllocator* heap = hint ? m->VirtualFor(hint) : (alloc_type & kMemLargePages) ? &m->virt : &m->virt4k;
  if (!heap) return kStatusInvalidParameter;
  uint32_t size = *size_ptr;
  if (int32_t(size) < 0) size = 0u - size;  // Xenia: some games pass negative sizes
  uint32_t out_size = 0;
  const uint32_t addr = heap->Allocate(hint, size, alloc_type, NormalizeProtect(protect), &out_size);
  if (!addr) return kStatusNoMemory;
  *base_ptr = addr;
  *size_ptr = out_size;
  return kStatusSuccess;
}

// Xenia: NtFreeVirtualMemory_entry. Release only from the allocation's base.
// Like Xenia (LookupHeap) it also applies to physical blocks, from any
// mirror (*base_ptr gets the address of the mirror used). Deviation:
// the process heap's arena (kernel-owned) is neither freed nor decommitted:
// STATUS_INVALID_PARAMETER, logged once.
uint32_t NtFreeVirtualMemoryImpl(be<uint32_t>* base_ptr, be<uint32_t>* size_ptr, uint32_t free_type,
                                 uint32_t /*debug_memory*/) {
  if (!base_ptr || !*base_ptr) return kStatusMemoryNotAllocated;
  MemorySystem* m = Memory();
  if (!m) return kStatusInvalidParameter;
  const uint32_t addr = *base_ptr;
  const uint32_t size = size_ptr ? uint32_t(*size_ptr) : 0;
  uint32_t out_size = 0;
  if (VirtualAllocator* heap = m->VirtualFor(addr)) {
    if (heap->IsKernelOwned(addr)) {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "[kernel] NtFreeVirtualMemory(0x%08X, type 0x%X) on the process heap arena: rejected",
                      addr, free_type);
        LogLine(line);
      }
      return kStatusInvalidParameter;
    }
    if (!heap->Free(addr, size, free_type, &out_size)) return kStatusUnsuccessful;
  } else {
    const uint32_t phys = CanonicalPhysicalAddress(addr);
    if (!m->phys.Contains(phys)) return kStatusInvalidParameter;
    if (!m->phys.Free(phys, size, free_type, &out_size)) return kStatusUnsuccessful;
  }
  *base_ptr = free_type == kMemDecommit ? addr & ~0xFFFu : addr;
  if (size_ptr) *size_ptr = out_size;
  return kStatusSuccess;
}

struct MemoryBasicInformation {
  be<uint32_t> base_address;
  be<uint32_t> allocation_base;
  be<uint32_t> allocation_protect;
  be<uint32_t> region_size;
  be<uint32_t> state;
  be<uint32_t> protect;
  be<uint32_t> type;
};
static_assert(sizeof(MemoryBasicInformation) == 28);

// Xenia: NtQueryVirtualMemory_entry. Deviation: besides the virtual heap it
// only answers for physical heap blocks; elsewhere (image, runtime
// area) STATUS_INVALID_PARAMETER. The 0xC/0xE mirrors give the same
// block as the 0xA view, with base_address and allocation_base translated to the
// queried view (the game compares them with its own pointers).
uint32_t NtQueryVirtualMemoryImpl(uint32_t addr, MemoryBasicInformation* out) {
  const uint32_t view = addr >= 0xC0000000u ? addr & 0xE0000000u : 0;
  addr = CanonicalPhysicalAddress(addr);
  MemorySystem* m = Memory();
  if (!m || !out) return kStatusInvalidParameter;
  RegionInfo info;
  if (VirtualAllocator* heap = m->VirtualFor(addr)) {
    if (!heap->Query(addr, info)) return kStatusInvalidParameter;
  } else {
    uint32_t base = 0, size = 0, protect = 0;
    if (!m->phys.Find(addr, base, size, protect)) return kStatusInvalidParameter;
    info.base_address = InView(view, addr & ~0xFFFu);
    info.allocation_base = InView(view, base);
    info.allocation_protect = protect;
    info.region_size = uint32_t(AlignUp(uint64_t(base) + size, 0x1000) - (addr & ~0xFFFu));
    info.state = kMemCommit;
    info.protect = protect;
  }
  out->base_address = info.base_address;
  out->allocation_base = info.allocation_base;
  out->allocation_protect = info.allocation_protect;
  out->region_size = info.region_size;
  out->state = info.state;
  out->protect = info.protect;
  out->type = kMemPrivate;
  return kStatusSuccess;
}

// Xenia: NtProtectVirtualMemory_entry. Deviation: the protection is only
// recorded (queries report it), the host pages stay readable/writable.
uint32_t NtProtectVirtualMemoryImpl(be<uint32_t>* base_ptr, be<uint32_t>* size_ptr, uint32_t protect,
                                    be<uint32_t>* old_protect, uint32_t /*debug_memory*/) {
  if (!base_ptr || !size_ptr || !*size_ptr) return kStatusInvalidParameter;
  if (protect & kPageExecuteAny) return kStatusAccessDenied;
  MemorySystem* m = Memory();
  const uint32_t addr = *base_ptr;
  VirtualAllocator* heap = m ? m->VirtualFor(addr) : nullptr;
  if (!heap) return kStatusInvalidParameter;
  const uint32_t size = *size_ptr;
  uint32_t old = 0;
  if (!heap->Protect(addr, size, NormalizeProtect(protect), &old)) return kStatusAccessDenied;
  const uint32_t base = addr & ~0xFFFu;
  *base_ptr = base;
  *size_ptr = uint32_t(AlignUp(uint64_t(addr) + size, 0x1000) - base);
  if (old_protect) *old_protect = old;
  return kStatusSuccess;
}

// Xenia: MmAllocatePhysicalMemoryEx_entry. Deviations: a single physical heap
// (0xA0000000-0xBFFFFFFF, 4 KB pages) for all page sizes,
// which decide rounding, minimum alignment (on the physical address, as in
// Xenia) and the view of the returned address, like Xenia's heaps: 4 KB ->
// 0xE0000000 + physical - 0x1000 (heap vE0000000 of 0x1FD00000 bytes: physical in
// [0x1000, 0x1FD01000)), 64 KB -> 0xA0000000 + physical, 16 MB -> 0xC0000000 +
// physical. UE3 (appFree) picks how to free a block from the high bits
// of the address. The 512 MiB budget yields 0. The physical range [min, max]
// is translated to heap addresses as in Xenia.
uint32_t MmAllocatePhysicalMemoryExImpl(uint32_t /*flags*/, uint32_t size, uint32_t protect, uint32_t min_addr,
                                        uint32_t max_addr, uint32_t alignment) {
  if (!(protect & (kPageReadOnly | kPageReadWrite))) return 0;
  MemorySystem* m = Memory();
  if (!m || size == 0) return 0;
  uint32_t page = 0x1000;
  uint32_t view = gow2::kViewEBegin;
  uint32_t phys_low = 0, phys_high = kPhysicalHeapSize - 1;
  if (protect & kMemLargePages) {
    page = 0x10000;
    view = 0xA0000000;
  } else if (protect & kMem16MbPages) {
    page = 0x1000000;
    view = 0xC0000000;
  } else {
    // 0xE view: physical below 0x1000 has no address, and Xenia's heap
    // ends at 0xFFD00000 (never the last page, never across a boundary)
    phys_low = gow2::kViewEOffset;
    phys_high = 0x1FD00000u + gow2::kViewEOffset - 1;
  }
  const uint64_t adjusted_size = AlignUp(size, page);
  uint64_t adjusted_align = std::max<uint64_t>(AlignUp(alignment, page), page);
  while (adjusted_align & (adjusted_align - 1)) adjusted_align += adjusted_align & -adjusted_align;  // power of 2
  if (adjusted_size > kPhysicalHeapSize || adjusted_align > kPhysicalHeapSize) return 0;
  const uint32_t low_phys = std::max(min_addr, phys_low), high_phys = std::min(max_addr, phys_high);
  if (low_phys > high_phys) return 0;
  const uint32_t low = kPhysicalHeapBegin + low_phys, high = kPhysicalHeapBegin + high_phys;
  const uint32_t addr =
      m->phys.Alloc(uint32_t(adjusted_size), uint32_t(adjusted_align), NormalizeProtect(protect), low, high);
  return addr ? InView(view, addr) : 0;
}

// Xenia: MmAllocatePhysicalMemory_entry.
uint32_t MmAllocatePhysicalMemoryImpl(uint32_t flags, uint32_t size, uint32_t protect) {
  return MmAllocatePhysicalMemoryExImpl(flags, size, protect, 0, 0xFFFFFFFF, 0);
}

// Xenia: MmFreePhysicalMemory_entry. From any mirror.
void MmFreePhysicalMemoryImpl(uint32_t /*type*/, uint32_t addr) {
  if (MemorySystem* m = Memory()) m->phys.Free(CanonicalPhysicalAddress(addr));
}

// Xenia: MmGetPhysicalAddress_entry (PhysicalHeap::GetPhysicalAddress):
// addr & 0x1FFFFFFF, + 0x1000 from the 0xE view.
uint32_t MmGetPhysicalAddressImpl(uint32_t addr) { return gow2::PhysicalAddressOf(addr); }

// Xenia: MmQueryAddressProtect_entry. Deviation: image and runtime area
// (0x80000000-0x9FFFFFFF) are not tracked and count as PAGE_READWRITE.
uint32_t MmQueryAddressProtectImpl(uint32_t addr) {
  addr = CanonicalPhysicalAddress(addr);
  MemorySystem* m = Memory();
  if (!m) return 0;
  if (VirtualAllocator* heap = m->VirtualFor(addr)) return heap->QueryProtect(addr);
  uint32_t base = 0, size = 0, protect = 0;
  if (m->phys.Find(addr, base, size, protect)) return protect;
  if (addr >= 0x80000000 && addr < kPhysicalHeapBegin) return kPageReadWrite;
  return 0;
}

// Xenia: MmSetAddressProtect_entry. Only recorded, like NtProtectVirtualMemory.
void MmSetAddressProtectImpl(uint32_t addr, uint32_t size, uint32_t protect) {
  addr = CanonicalPhysicalAddress(addr);
  MemorySystem* m = Memory();
  if (!m || !protect) return;
  if (VirtualAllocator* heap = m->VirtualFor(addr))
    heap->Protect(addr, size, NormalizeProtect(protect));
  else
    m->phys.SetProtect(addr, NormalizeProtect(protect));
}

// Xenia: MmQueryAllocationSize_entry.
uint32_t MmQueryAllocationSizeImpl(uint32_t addr) {
  addr = CanonicalPhysicalAddress(addr);
  MemorySystem* m = Memory();
  if (!m) return 0;
  if (VirtualAllocator* heap = m->VirtualFor(addr)) return heap->AllocationSize(addr);
  uint32_t base = 0, size = 0, protect = 0;
  return m->phys.Find(addr, base, size, protect) ? size : 0;
}

struct MmStatisticsSection {
  be<uint32_t> available_pages;
  be<uint32_t> total_virtual_memory_bytes;
  be<uint32_t> reserved_virtual_memory_bytes;
  be<uint32_t> physical_pages;
  be<uint32_t> pool_pages;
  be<uint32_t> stack_pages;
  be<uint32_t> image_pages;
  be<uint32_t> heap_pages;
  be<uint32_t> virtual_pages;
  be<uint32_t> page_table_pages;
  be<uint32_t> cache_pages;
};
struct MmStatistics {
  be<uint32_t> size;
  be<uint32_t> total_physical_pages;
  be<uint32_t> kernel_pages;
  MmStatisticsSection title;
  MmStatisticsSection system;
  be<uint32_t> highest_physical_page;
};
static_assert(sizeof(MmStatistics) == 104);

// Xenia: MmQueryStatistics_entry. The constants are Xenia's; pages
// in use come from the budget (BytesInUse) rather than from the heaps' pages.
uint32_t MmQueryStatisticsImpl(MmStatistics* s) {
  if (!s) return kStatusInvalidParameter;
  if (s->size != sizeof(MmStatistics)) return kStatusBufferTooSmall;
  std::memset(s, 0, sizeof(MmStatistics));
  constexpr uint32_t kTotalPages = uint32_t(kBudget / 0x1000);  // 0x20000
  const uint32_t used = uint32_t(std::min<uint64_t>(AlignUp(BytesInUse(), 0x1000) / 0x1000, kTotalPages));
  s->size = sizeof(MmStatistics);
  s->total_physical_pages = kTotalPages;
  s->kernel_pages = 0x300;
  s->title.available_pages = kTotalPages - used;
  s->title.total_virtual_memory_bytes = 0x2FFF0000;
  s->title.reserved_virtual_memory_bytes = 0x00160000;
  s->title.physical_pages = 0x1000;
  s->title.pool_pages = 0x10;
  s->title.stack_pages = 0x100;
  s->title.image_pages = 0x100;
  s->title.heap_pages = 0x100;
  s->title.virtual_pages = 0x100;
  s->title.page_table_pages = 0x100;
  s->title.cache_pages = 0x100;
  s->highest_physical_page = 0x1FFFF;
  return kStatusSuccess;
}

// Unleashed heap.cpp: RtlAllocateHeap (HEAP_ZERO_MEMORY zeroes); 0 if exhausted
// instead of assert.
uint32_t RtlAllocateHeapImpl(uint32_t /*heap*/, uint32_t flags, uint32_t size) {
  MemorySystem* m = Memory();
  const uint32_t addr = m ? m->heap.Alloc(size) : 0;
  if (addr && (flags & kHeapZeroMemory)) std::memset(g_base + addr, 0, size);
  return addr;
}

// Unleashed heap.cpp: RtlFreeHeap, always TRUE.
uint32_t RtlFreeHeapImpl(uint32_t /*heap*/, uint32_t /*flags*/, uint32_t addr) {
  if (MemorySystem* m = Memory(); m && addr) m->heap.Free(addr);
  return 1;
}

// Unleashed heap.cpp: RtlSizeHeap, the requested size (0 if not a block).
uint32_t RtlSizeHeapImpl(uint32_t /*heap*/, uint32_t /*flags*/, uint32_t addr) {
  MemorySystem* m = Memory();
  return m && addr ? m->heap.Size(addr) : 0;
}

// Unleashed heap.cpp: RtlReAllocateHeap. Contents preserved; with
// HEAP_ZERO_MEMORY the new part is zeroed; on failure the old block
// stays valid and the result is 0.
uint32_t RtlReAllocateHeapImpl(uint32_t /*heap*/, uint32_t flags, uint32_t addr, uint32_t size) {
  MemorySystem* m = Memory();
  if (!m) return 0;
  const uint32_t old = addr ? m->heap.Size(addr) : 0;
  const uint32_t moved = m->heap.Realloc(addr, size);
  if (moved && (flags & kHeapZeroMemory) && size > old) std::memset(g_base + moved + old, 0, size - old);
  return moved;
}

}  // namespace

KERNEL_EXPORT(NtAllocateVirtualMemory, NtAllocateVirtualMemoryImpl)
KERNEL_EXPORT(NtFreeVirtualMemory, NtFreeVirtualMemoryImpl)
KERNEL_EXPORT(NtQueryVirtualMemory, NtQueryVirtualMemoryImpl)
KERNEL_EXPORT(NtProtectVirtualMemory, NtProtectVirtualMemoryImpl)
KERNEL_EXPORT(MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryExImpl)
KERNEL_EXPORT(MmAllocatePhysicalMemory, MmAllocatePhysicalMemoryImpl)
KERNEL_EXPORT(MmFreePhysicalMemory, MmFreePhysicalMemoryImpl)
KERNEL_EXPORT(MmGetPhysicalAddress, MmGetPhysicalAddressImpl)
KERNEL_EXPORT(MmQueryAddressProtect, MmQueryAddressProtectImpl)
KERNEL_EXPORT(MmSetAddressProtect, MmSetAddressProtectImpl)
KERNEL_EXPORT(MmQueryAllocationSize, MmQueryAllocationSizeImpl)
KERNEL_EXPORT(MmQueryStatistics, MmQueryStatisticsImpl)
KERNEL_EXPORT(RtlAllocateHeap, RtlAllocateHeapImpl)
KERNEL_EXPORT(RtlFreeHeap, RtlFreeHeapImpl)
KERNEL_EXPORT(RtlSizeHeap, RtlSizeHeapImpl)
KERNEL_EXPORT(RtlReAllocateHeap, RtlReAllocateHeapImpl)
