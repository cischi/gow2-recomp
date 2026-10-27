// Redefinitions of the ppc_context.h (XenonUtils) macros for the recompiled
// code. Included by build/generated/ppc_config.h when PPC_INCLUDE_DETAIL is
// defined (runtime/CMakeLists.txt, on gow2_recomp and gow2), therefore
// before ppc_context.h, which defines its own versions only under #ifndef.
//
// PPC_HOST: host pointer of the guest address for every access of the
// generated code (PPC_LOAD/STORE macros and XenonRecomp's direct forms, patch
// 0003), with the 0xE0000000 view shift (guest_address.h).
//
// PPC_MM_STORE_U32: XenonRecomp emits it for a store followed by eieio, i.e.
// the game's MMIO writes (12 sites: 5 in the title's XMA library,
// registers 0x7FEAxxxx, and 7 on the GPU registers 0x7FC8xxxx). The memory store
// stays identical to PPC_STORE_U32; if the address falls in the XMA registers
// (0x7FEA0000-0x7FEAFFFF) the call to gow2_mmio_store32 follows with the
// little-endian DWORD just written, i.e. the register's natural value: for
// stwbrx (y already swapped by the generator) and for stwx alike.
// `x` and `y` are evaluated only once.
//
// PPC_QUERY_TIMEBASE: timebase read for mftb (patch 0004; XenonRecomp's default
// is __rdtsc(), the PC's TSC at a few GHz). The game assumes the console's
// 50 MHz (KeQueryPerformanceFrequency = 50'000'000): the runtime function
// returns plat::Ticks50MHz(), monotonic and lock-free.
#pragma once

#include <cstdint>

#include "guest_address.h"

// runtime/kernel/xma.cpp: game write to an XMA register.
extern "C" void gow2_mmio_store32(uint32_t guest_address, uint32_t value);

// runtime/kernel/rtl_exports.cpp: guest timebase at 50 MHz.
extern "C" uint64_t gow2_query_timebase();

#define PPC_QUERY_TIMEBASE() gow2_query_timebase()

#define PPC_HOST(x) (base + ::gow2::HostOffset(static_cast<uint32_t>(x)))

#define PPC_MM_STORE_U32(x, y)                                                          \
  do {                                                                                  \
    const uint32_t gow2_mm_address_ = static_cast<uint32_t>(x);                         \
    const uint32_t gow2_mm_stored_ = __builtin_bswap32(static_cast<uint32_t>(y));       \
    *(volatile uint32_t*)PPC_HOST(gow2_mm_address_) = gow2_mm_stored_;                  \
    if ((gow2_mm_address_ & 0xFFFF0000u) == 0x7FEA0000u)                                \
      gow2_mmio_store32(gow2_mm_address_, gow2_mm_stored_);                             \
  } while (0)
