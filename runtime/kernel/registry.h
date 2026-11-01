// Registry of the kernel/XAM import implementations.
//
//   uint32_t NtCloseImpl(uint32_t handle) { ... }
//   KERNEL_EXPORT(NtClose, NtCloseImpl)
//
// defines __imp__NtClose (declared by XenonRecomp in ppc_recomp_shared.h)
// as a call to NtCloseImpl, with the arguments translated by function.h.
// tools/gen_import_stubs.py looks for `KERNEL_EXPORT(Name` in the .cpp files of
// runtime/kernel/ and does not generate the log stub for those names: an import is
// implemented exactly when it appears here.
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "kernel/function.h"
#include "kernel/stats.h"

namespace kernel {
// Guest thread suspension (NtSuspendThread on another thread): the
// thread stops at its next export. t_suspend_count points to the suspend
// counter of the current GuestThread (nullptr on host threads);
// SuspendSafePoint (threads.cpp) waits for it to return to 0.
inline thread_local std::atomic<int32_t>* t_suspend_count = nullptr;
void SuspendSafePoint();
inline void CheckSuspend() {
  if (std::atomic<int32_t>* c = t_suspend_count; c && c->load(std::memory_order_relaxed) > 0) SuspendSafePoint();
}

// Guest return address (lr) of the last export call made by this
// thread: KERNEL_EXPORT records it before calling the implementation,
// which uses it to log the caller (unknown handles, impossible states).
inline thread_local uint32_t t_export_caller = 0;
inline uint32_t ExportCaller() { return t_export_caller; }

// Every KERNEL_EXPORT / KERNEL_EXPORT_RAW registers its __imp__ function by
// name at static initialization: XexGetProcedureAddress on a kernel module
// (module.cpp) hands out the exports a title resolves at run time.
bool RegisterExport(const char* name, PPCFunc* func);
PPCFunc* FindExport(std::string_view name);  // nullptr if not implemented

namespace stats {
// --trace: writes "[kernel] Name(r3=0x..., ..., r10=0x...)" (stats.cpp).
void TraceCall(const char* name, const PPCContext& ctx);
}  // namespace stats
}  // namespace kernel

#define KERNEL_EXPORT(Name, HostFunc)                              \
  PPC_EXTERN_FUNC(__imp__##Name);                                  \
  [[maybe_unused]] static const bool Name##_registered = ::kernel::RegisterExport(#Name, __imp__##Name); \
  PPC_FUNC(__imp__##Name) {                                        \
    ::kernel::CheckSuspend();                                      \
    ::kernel::t_export_caller = static_cast<uint32_t>(ctx.lr);         if (::kernel::stats::Trace()) ::kernel::stats::TraceCall(#Name, ctx); \
    HostToGuestFunction<HostFunc>(ctx, base);                      \
  }

// For imports that need the whole PPCContext (e.g. lr of
// KeBugCheck): KERNEL_EXPORT_RAW(Name) { ...body with ctx and base... }
#define KERNEL_EXPORT_RAW(Name)                                                      static void Name##_raw_body(PPCContext& ctx, uint8_t* base);   PPC_EXTERN_FUNC(__imp__##Name);   [[maybe_unused]] static const bool Name##_registered = ::kernel::RegisterExport(#Name, __imp__##Name);                       PPC_FUNC(__imp__##Name) {                                                            if (::kernel::stats::Trace()) ::kernel::stats::TraceCall(#Name, ctx);              Name##_raw_body(ctx, base);                                                      }                                                                                  static void Name##_raw_body(PPCContext& ctx, uint8_t* base)
