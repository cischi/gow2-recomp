// Translation between guest addresses (32 bit) and host pointers in GuestMemory's
// 4 GB arena: guest address A lives at g_base + gow2::HostOffset(A), i.e.
// g_base + A except in the 0xE0000000 view (+0x1000, guest_address.h).
#pragma once

#include <cstdint>

#include "guest_address.h"

namespace kernel {

// Base of the guest arena. Set by main right after GuestMemory::Init (tests
// point it at a scratch arena); it is the same `base` passed to the generated
// code.
inline uint8_t* g_base = nullptr;

// Host pointer for guest address `addr`; nullptr if addr == 0.
template <class T>
T* GuestPtr(uint32_t addr) {
  return addr ? reinterpret_cast<T*>(g_base + gow2::HostOffset(addr)) : nullptr;
}

// Size of the guest space (= GuestMemory::kSize): beyond it lies host memory
// (apart from the uncommitted reserve after 4 GB for the last page of 0xE).
inline constexpr uint64_t kGuestSpaceSize = 0x1'0000'0000ull;

// true if [addr, addr + len) lies inside the 4 GiB guest space. Must be
// checked before writing/reading `len` bytes with a guest-chosen length:
// past the end of the arena the host would write into its own memory.
// len == 0 is always valid. The guest-side limit is enough: with the 0xE view
// offset the host range [HostOffset(addr), HostOffset(addr) + len) ends at most
// at 4 GB + 0x1000, inside GuestMemory's uncommitted tail reserve
// (kTailReserve = 64 KB): an overrun is a fault in the arena.
// The range is host-contiguous only if it does not cross 0xE0000000.
inline bool GuestRangeValid(uint32_t addr, uint64_t len) {
  return len <= kGuestSpaceSize - addr;
}

// Guest address of a host pointer inside the arena; 0 if host == nullptr.
inline uint32_t GuestAddr(const void* host) {
  return host ? gow2::GuestFromHostOffset(uint64_t(static_cast<const uint8_t*>(host) - g_base)) : 0;
}

}  // namespace kernel
