// Map from a host PC (address in the recompiled code) to the guest function
// that contains it, for the crash handler.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "platform/platform.h"

// Loads the map from PPCFuncMappings (generated code). As the end of the code it
// uses the last function + the largest gap between two consecutive functions
// (function sizes are not known). Defined in recomp_glue.cpp, therefore
// available only in the gow2 executable.
void CrashMapInit();
// Loads the map from {guest, host} pairs in any order (replaces the previous
// one). `host_end`: first host address after the mapped code.
void CrashMapInitFrom(std::span<const std::pair<uint32_t, uintptr_t>> guest_to_host,
                      uintptr_t host_end = UINTPTR_MAX);
// Guest address of the host function with the highest address <= pc, or 0 if pc
// is outside [first function, host_end).
uint32_t GuestFunctionForHostPc(uintptr_t pc);

// Writes the crash line into buf (truncating, always NUL-terminated):
//   crash host=0x… guest_func=0x…|? code=0x…[ addr=0x…][ stack overflow]
// "stack overflow" if the fault address is in [guard_begin, guard_end)
// (host addresses of the guard below the guest stack). Does not allocate: it is
// used inside the exception filter.
void FormatCrashLine(char* buf, size_t size, const plat::CrashInfo& info, uintptr_t guard_begin,
                     uintptr_t guard_end);
// As above, with "stack overflow" if the address is in one of the guards
// registered with RegisterStackGuard (one per guest thread).
void FormatCrashLine(char* buf, size_t size, const plat::CrashInfo& info);

// Guest stack guards (host addresses [begin, end)), read without locks by the
// crash handler. At most kMaxStackGuards at a time: RegisterStackGuard returns
// false if the table is full.
inline constexpr size_t kMaxStackGuards = 512;
bool RegisterStackGuard(uintptr_t begin, uintptr_t end);
void UnregisterStackGuard(uintptr_t begin);
bool InStackGuard(uintptr_t address);
