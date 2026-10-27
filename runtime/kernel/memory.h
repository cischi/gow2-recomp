// Guest memory of the HLE kernel: virtual heap (NtAllocateVirtualMemory),
// physical heap (MmAllocatePhysicalMemoryEx), process heap (RtlAllocateHeap)
// and a 512 MiB budget, the console's RAM.
//
// Layout:
//   virtual heap   0x40000000-0x7FC7FFFF  (below the GPU registers 0x7FC80000-0x7FCFFFFF)
//   physical heap  0xA0000000-0xBFFFFFFF (the same bytes also from 0xC0000000
//                  and 0xE0000000: GuestMemory maps three views of physical memory,
//                  the 0xE one with the 4 KB offset of guest_address.h).
//                  MmAllocatePhysicalMemoryEx returns the view matching the
//                  page size: 4 KB -> 0xE, 64 KB -> 0xA, 16 MB -> 0xC.
// GuestMemory's 4 GB arena is only reserved: the allocators commit the pages
// they hand out and decommit them when freed, so an access to unallocated
// memory fails with a logged crash.
//
// Budget: BytesInUse() = committed pages of the virtual heap (excluding the
// process heap's arena) + physical heap blocks + process heap blocks.
// An allocation that would push it past kBudget fails (0 /
// STATUS_NO_MEMORY) with no side effects.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct GuestMemory;
struct O1HeapInstance;

namespace kernel {

inline constexpr uint64_t kBudget = 512ull << 20;

inline constexpr uint32_t kVirtualHeapBegin = 0x40000000;
inline constexpr uint32_t kVirtualHeapEnd = 0x7FC80000;  // exclusive
// Virtual heap with 4 KB pages (Xenia heaps_.v00000000, 0x00000000-0x3FFFFFFF):
// NtAllocateVirtualMemory without X_MEM_LARGE_PAGES. UE3 tells apart, by address
// (< 0x40000000), blocks taken with VirtualAlloc from those of its pools.
// Starts at 0x10000000: below it lies the space of GuestMemory's bump
// allocator (kDefaultHeapStart) used by tests before LoadImage.
inline constexpr uint32_t kVirtual4kHeapBegin = 0x10000000;
inline constexpr uint32_t kVirtual4kHeapEnd = 0x40000000;  // exclusive
inline constexpr uint32_t kPhysicalHeapBegin = 0xA0000000;
inline constexpr uint64_t kPhysicalHeapEnd = 0xC0000000;  // exclusive
// Process heap arena (o1heap), taken from the top of the virtual heap.
inline constexpr uint32_t kProcessHeapArenaSize = 256u << 20;

// Console constants (Xenia src/xenia/xbox.h).
inline constexpr uint32_t kMemCommit = 0x00001000;
inline constexpr uint32_t kMemReserve = 0x00002000;
inline constexpr uint32_t kMemDecommit = 0x00004000;
inline constexpr uint32_t kMemRelease = 0x00008000;
inline constexpr uint32_t kMemFree = 0x00010000;
inline constexpr uint32_t kMemPrivate = 0x00020000;
inline constexpr uint32_t kMemReset = 0x00080000;
inline constexpr uint32_t kMemTopDown = 0x00100000;
inline constexpr uint32_t kMemNoZero = 0x00800000;
inline constexpr uint32_t kMemLargePages = 0x20000000;
inline constexpr uint32_t kMemHeap = 0x40000000;
inline constexpr uint32_t kMem16MbPages = 0x80000000;

inline constexpr uint32_t kPageNoAccess = 0x001;
inline constexpr uint32_t kPageReadOnly = 0x002;
inline constexpr uint32_t kPageReadWrite = 0x004;
inline constexpr uint32_t kPageExecuteRead = 0x020;
inline constexpr uint32_t kPageExecuteReadWrite = 0x040;
inline constexpr uint32_t kPageNoCache = 0x200;
inline constexpr uint32_t kPageWriteCombine = 0x400;

// Physical memory mirrors: 0xC0000000-0xDFFFFFFF and 0xE0000000-0xFFFFFFFF
// are the same bytes as 0xA0000000-0xBFFFFFFF (GuestMemory maps three views;
// from 0xE the physical address has Xenia's +0x1000 offset: gow2::PhysicalAddressOf).
// Returns the address of the same physical byte in the 0xA view, which is the
// one of the physical heap (the last page of 0xE, physical 0x20000000, becomes
// 0xC0000000-0xC0000FFF: no block); other addresses stay
// unchanged. The physical and query exports apply it on entry.
uint32_t CanonicalPhysicalAddress(uint32_t guest);

// Bytes in use against kBudget (sum over all live allocators).
uint64_t BytesInUse();

// State of a homogeneous region (same pages, state and protection) like
// X_MEMORY_BASIC_INFORMATION.
struct RegionInfo {
  uint32_t base_address = 0;
  uint32_t allocation_base = 0;
  uint32_t allocation_protect = 0;
  uint32_t region_size = 0;
  uint32_t state = 0;    // kMemCommit | kMemReserve | kMemFree
  uint32_t protect = 0;  // 0 if not committed
};

// Heap with 4 KB pages (Xenia BaseHeap, src/xenia/memory.cc). Reservations
// start at multiples of `granularity` (64 KB for the virtual heap); the
// size is rounded to 64 KB with kMemLargePages, otherwise to 4 KB
// (Xenia NtAllocateVirtualMemory_entry). Thread-safe.
class VirtualAllocator {
 public:
  VirtualAllocator(GuestMemory&, uint32_t begin, uint32_t end, uint32_t granularity = 0x10000);
  ~VirtualAllocator();
  VirtualAllocator(const VirtualAllocator&) = delete;
  VirtualAllocator& operator=(const VirtualAllocator&) = delete;

  // kMemReserve and/or kMemCommit (kMemTopDown, kMemLargePages allowed). With
  // hint != 0: inside an existing reservation it commits [hint, hint+size)
  // rounded to pages; on free pages it creates a reservation from
  // hint rounded to 64 KB. Returns the base (0 on failure, including
  // budget) and, in *out_size, the actual size.
  uint32_t Allocate(uint32_t hint, uint32_t size, uint32_t alloc_type, uint32_t protect,
                    uint32_t* out_size = nullptr);
  // kMemDecommit: decommits [addr, addr+size) (size 0 = up to the end
  // of the allocation). kMemRelease: frees the whole allocation starting at
  // addr; its size goes in *out_size.
  bool Free(uint32_t addr, uint32_t size, uint32_t free_type, uint32_t* out_size = nullptr);
  bool Query(uint32_t addr, RegionInfo&) const;
  // Recorded protection (not applied to the host) of committed pages of
  // a single allocation; *old gets the first page's.
  bool Protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* old = nullptr);
  uint32_t QueryProtect(uint32_t addr) const;   // 0 if not committed
  uint32_t AllocationSize(uint32_t addr) const;  // 0 if free
  bool Contains(uint32_t addr) const { return addr >= begin_ && addr < end_; }

  // Reserve+commit of an arena for an internal allocator (process heap):
  // does not count against the budget, which counts the allocator's blocks. The arena is "owned
  // by the kernel": Free rejects it (only the destructor releases it). 0 on failure.
  uint32_t AllocateArena(uint32_t size);
  // true if addr lies in an arena created by AllocateArena.
  bool IsKernelOwned(uint32_t addr) const;

  // Reserve+commit (counted against the budget) of `size` bytes (rounded to 4 KB) at an
  // address that is a multiple of `align` (power of 2, at least 4 KB) inside
  // [low, high], searching from the top if top_down (Xenia BaseHeap::AllocRange).
  // 0 on failure.
  uint32_t AllocateRange(uint32_t low, uint32_t high, uint32_t size, uint32_t align, uint32_t protect,
                         bool top_down);

 private:
  struct Page {
    uint32_t alloc_base = 0;  // 0 = free
    uint32_t protect = 0;
    bool committed = false;
  };
  struct Allocation {
    uint32_t size;
    uint32_t protect;
    bool charged;  // committed pages count against the budget
    bool kernel;   // AllocateArena arena: the guest does not free it
  };
  size_t Index(uint32_t addr) const { return (addr - begin_) >> 12; }
  // First (or last) address that is a multiple of align in [low, high) with `size`
  // bytes of free pages; 0 if none.
  uint32_t FindFree(uint64_t low, uint64_t high, uint32_t size, uint32_t align, bool top_down) const;
  uint32_t Reserve(uint32_t base, uint64_t length, uint32_t alloc_type, uint32_t protect, bool charged,
                   uint32_t* out_size, bool kernel = false);
  bool CommitRange(uint32_t addr, uint32_t size, uint32_t protect, bool charged);
  void DecommitRange(uint32_t addr, uint32_t size, bool charged);
  uint32_t AllocateLocked(uint32_t hint, uint32_t size, uint32_t alloc_type, uint32_t protect, bool charged,
                          uint32_t* out_size, bool kernel = false);

  GuestMemory& mem_;
  uint32_t begin_, end_, granularity_;
  mutable std::mutex mutex_;
  std::vector<Page> pages_;
  std::map<uint32_t, Allocation> allocs_;
};

// Physical heap on 0xA0000000-0xBFFFFFFF: table of 4 KB pages (the same
// VirtualAllocator, 4 KB granularity; Xenia PhysicalHeap/BaseHeap,
// src/xenia/memory.cc). Each block is committed on allocation (zeroed) and
// decommitted on free; it counts against the budget for its pages, with no
// power-of-2 waste. Thread-safe.
class PhysicalHeap {
 public:
  explicit PhysicalHeap(GuestMemory&);
  PhysicalHeap(const PhysicalHeap&) = delete;
  PhysicalHeap& operator=(const PhysicalHeap&) = delete;

  // 0 on failure; size rounded to 4 KB; align 0 -> 0x1000 (minimum 4 KB).
  // [low, high]: range of guest addresses the block must lie in.
  uint32_t Alloc(uint32_t size, uint32_t align, uint32_t protect = kPageReadWrite,
                 uint32_t low = kPhysicalHeapBegin, uint32_t high = uint32_t(kPhysicalHeapEnd - 1));
  void Free(uint32_t addr);
  // Like VirtualAllocator::Free (NtFreeVirtualMemory on a physical block):
  // kMemRelease from the base, kMemDecommit of a range. addr in the 0xA view.
  bool Free(uint32_t addr, uint32_t size, uint32_t free_type, uint32_t* out_size);
  uint32_t Size(uint32_t addr) const;  // 0 if addr is not the start of a block
  // Block containing addr: base, size and protection; false if none.
  bool Find(uint32_t addr, uint32_t& base, uint32_t& size, uint32_t& protect) const;
  bool SetProtect(uint32_t addr, uint32_t protect);
  bool Contains(uint32_t addr) const { return addr >= kPhysicalHeapBegin && addr < kPhysicalHeapEnd; }

 private:
  VirtualAllocator pages_;
};

// Process heap (RtlAllocateHeap & co., Rtl's system heap): o1heap on
// an arena of the virtual heap. Thread-safe.
class ProcessHeap {
 public:
  ProcessHeap(GuestMemory&, VirtualAllocator&, uint32_t arena_size = kProcessHeapArenaSize);
  ~ProcessHeap();
  ProcessHeap(const ProcessHeap&) = delete;
  ProcessHeap& operator=(const ProcessHeap&) = delete;

  uint32_t Alloc(uint32_t size);  // 0 on failure
  void Free(uint32_t addr);
  uint32_t Size(uint32_t addr) const;  // 0 if addr is not a block
  // Contents preserved up to min(old, new) size; 0 on failure
  // (the old block stays valid). addr 0 = Alloc.
  uint32_t Realloc(uint32_t addr, uint32_t size);
  bool Valid() const { return heap_ != nullptr; }

 private:
  GuestMemory& mem_;
  O1HeapInstance* heap_ = nullptr;
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, uint32_t> sizes_;
};

// Allocators used by the exports (Nt*VirtualMemory, Mm*, Rtl*Heap). InitMemory
// creates them on `mem` and links the process heap to kernel::rtl::SetSystemHeap;
// a new call replaces the previous ones. ShutdownMemory destroys them.
struct MemorySystem {
  explicit MemorySystem(GuestMemory&);
  VirtualAllocator virt;    // 0x40000000-0x7FC7FFFF, 64 KB reservations
  VirtualAllocator virt4k;  // 0x10000000-0x3FFFFFFF, 4 KB reservations
  // Virtual heap containing addr (virt4k or virt), nullptr if none.
  VirtualAllocator* VirtualFor(uint32_t addr) {
    if (virt.Contains(addr)) return &virt;
    if (virt4k.Contains(addr)) return &virt4k;
    return nullptr;
  }
  PhysicalHeap phys;
  ProcessHeap heap;
};
bool InitMemory(GuestMemory&);
void ShutdownMemory();
MemorySystem* Memory();  // nullptr before InitMemory

}  // namespace kernel
