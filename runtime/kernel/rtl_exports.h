// Configuration points of rtl_exports.cpp (Rtl/Ke/Hal utilities).
#pragma once

#include <cstddef>
#include <cstdint>

namespace kernel::rtl {

// System heap from which RtlUnicodeStringToAnsiString(alloc=TRUE) takes the
// buffer and into which RtlFreeAnsiString puts it back (Xenia: SystemHeapAlloc /
// SystemHeapFree). Guest addresses; alloc returns 0 if exhausted. Until they
// are set (the heap arrives with the memory services) the allocation
// answers STATUS_NO_MEMORY and the free does nothing.
using SystemAllocFn = uint32_t (*)(uint32_t size);
using SystemFreeFn = void (*)(uint32_t guest_addr);
void SetSystemHeap(SystemAllocFn alloc, SystemFreeFn free);

// System flags of the XEX header (XEX_HEADER_SYSTEM_FLAGS, key 0x30000)
// that XexCheckExecutablePrivilege queries; 0 if the header is absent.
void SetExecutableSystemFlags(uint32_t flags);

// Called before _Exit by the kernel exits (HalReturnToFirmware, KeBugCheck): main.cpp uses it to stop
// an OptiScaler installation in progress. nullptr = none.
void SetBeforeExit(void (*fn)());

// Current time as a FILETIME (100 ns since 1601), the clock of
// KeQuerySystemTime: non-decreasing. The absolute timeouts of waits and
// timers (sync.cpp) use it too.
uint64_t QuerySystemTime();

// Routines registered with ExRegisterTitleTerminateNotification (and not removed).
size_t TitleTerminateNotificationCount();

}  // namespace kernel::rtl
