// Translation of a guest address to an offset in the host arena, with the 4 KB
// shift of the 0xE0000000 physical view.
//
// On the console (and in Xenia: PhysicalHeap::GetPhysicalAddress, memory.cc)
// the address 0xE0000000 + x is the physical byte x + 0x1000, while 0xA0000000 + x
// and 0xC0000000 + x are the physical byte x. The title's D3D code accounts for it
// when it passes addresses to the GPU: (x & 0x1FFFFFFF) + (((x >> 20) + 0x200) & 0x1000).
// GuestMemory's three views are mapped at 64 KB (Windows granularity),
// so the 0xE view starts at physical 0 like the others and the shift is applied
// here, at every translation: the guest address A >= 0xE0000000 lives at
// base + A + 0x1000. The last guest page (0xFFFFF000) thus falls beyond the
// 4 GB, in an uncommitted reserve of GuestMemory (fault logged).
#pragma once

#include <cstdint>

namespace gow2 {

inline constexpr uint32_t kViewEBegin = 0xE0000000u;
inline constexpr uint32_t kViewEOffset = 0x1000u;

// Host offset (to add to the arena base) of the guest address.
constexpr uint64_t HostOffset(uint32_t guest) {
  return uint64_t(guest) + (guest >= kViewEBegin ? kViewEOffset : 0u);
}

// Inverse of HostOffset for offsets produced by HostOffset. The offsets
// [0xE0000000, 0xE0001000) have no guest address (they are the physical
// 0-0xFFF seen from 0xE, which the guest only reaches from 0xA/0xC) and
// come back unchanged.
constexpr uint32_t GuestFromHostOffset(uint64_t offset) {
  return offset >= uint64_t(kViewEBegin) + kViewEOffset ? uint32_t(offset - kViewEOffset) : uint32_t(offset);
}

// Physical address (0-0x1FFFFFFF) of an address in the physical views
// (0xA0000000-0xFFFFFFFF), like MmGetPhysicalAddress. The 0xE view is
// shifted by 0x1000 (see above; the result stays within 512 MiB except
// for the last page, which stays at 0x20000000 as in Xenia).
constexpr uint32_t PhysicalAddressOf(uint32_t guest) {
  return (guest & 0x1FFFFFFFu) + (guest >= kViewEBegin ? kViewEOffset : 0u);
}

}  // namespace gow2
