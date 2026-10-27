#pragma once

#include <cstdint>

// 4 GB arena that represents the 32-bit guest address space:
// the guest address A lives at base + A (except for the 0xE view shift,
// below). The arena is only reserved: every
// access to an uncommitted page (by Commit, Alloc or the kernel's allocators)
// fails with a logged crash.
//
// Physical mirrors (Xenia src/xenia/memory.cc, map_info and MapViews): the 512 MiB
// physical memory is a shared section mapped three times, at 0xA0000000,
// 0xC0000000 and 0xE0000000, so 0xA0000000+p, 0xC0000000+p and 0xE0000000+p
// are the same physical byte p (host offset). Below 0xA0000000 there is a
// normal reserve.
// Shift of the 0xE view (as in Xenia, host_address_offset): the guest address
// 0xE0000000 + x is the physical byte x + 0x1000. MapViewOfFile3 wants 64 KB offsets,
// so the host 0xE view starts at physical 0 like the others and the shift is
// applied in the guest -> host translation (gow2::HostOffset, guest_address.h):
// the guest A >= 0xE0000000 lives at base + A + 0x1000. Commit and Decommit
// work on guest addresses and use gow2::PhysicalAddressOf. The last
// guest page (0xFFFFF000, physical 0x20000000) falls beyond 4 GB, in an
// uncommitted tail reserve of kTailReserve bytes: an access is a fault
// inside the arena, never host memory; Commit/Decommit that touch it fail.
// The physical section is fully committed by Init (like Xenia's
// SEC_COMMIT): the GPU never faults reading freed or never-allocated
// ranges; the kernel's budget stays logical. In the views Commit is the
// handover of an allocation: it invalidates the GPU (physwatch::Trigger, if the arena
// is attached) and zeroes the pages, even if they were already in use (the kernel's
// allocators only commit new pages). Decommit (free) invalidates the GPU and
// leaves the pages committed and readable, with their content.
struct GuestMemory {
  static constexpr uint64_t kSize = 0x1'0000'0000;
  // Uncommitted reserve at base + kSize (the last guest page of 0xE);
  // 64 KB, the granularity of Windows reserves.
  static constexpr uint64_t kTailReserve = 0x10000;
  // First physical view; the others are at +kPhysicalSize and +2*kPhysicalSize.
  static constexpr uint64_t kPhysicalViewBegin = 0xA0000000;
  static constexpr uint64_t kPhysicalSize = 0x20000000;
  static constexpr uint64_t kPreferredBase = 0x1'0000'0000;
  // Default heap start before LoadImage/SetHeapStart move it:
  // skips page 0 so no allocation is 0 (= failure).
  static constexpr uint32_t kDefaultHeapStart = 0x10000;
  // Limit of Alloc's runtime area: the physical heap starts at 0xA0000000.
  static constexpr uint64_t kAllocLimit = 0xA0000000;

  uint8_t* base = nullptr;
  void* physical_section = nullptr;

  GuestMemory() = default;
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  // Reserves kSize + kTailReserve bytes without committing them and maps the three
  // physical views of the section, fully committed. false on failure.
  bool Init();
  // Commits the 4 KB pages covering [guest, guest+size), zeroed if
  // new (in the physical views: always zeroed, all three views). false on
  // failure, if the range goes beyond 4 GB or touches the last guest page.
  bool Commit(uint32_t guest, uint32_t size);
  // Decommits the 4 KB pages covering [guest, guest+size) (in the physical
  // views: they stay committed and readable, see above). Fails like Commit.
  bool Decommit(uint32_t guest, uint32_t size);
  // Bump allocator for the runtime area (import variables, stacks, PCR):
  // returns the guest address aligned to `align` (power of 2; 0 means 1),
  // with the pages committed, or 0 if the allocation would exceed
  // kAllocLimit, align is invalid or the commit fails.
  uint32_t Alloc(uint32_t size, uint32_t align);
  // Moves the point where Alloc starts. LoadImage sets it to
  // align_up(image.base + image.size, 0x10000); the function table installation
  // then moves it past XenonRecomp's function table.
  void SetHeapStart(uint32_t guest_address);
  uint32_t HeapNext() const { return static_cast<uint32_t>(next_); }

 private:
  bool CommitPhysical(uint64_t phys, uint64_t length);
  bool DecommitPhysical(uint64_t phys, uint64_t length);
  uint64_t next_ = kDefaultHeapStart;
};
