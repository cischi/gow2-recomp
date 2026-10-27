// Kernel exports for guest threads: creation, end, suspension,
// affinity/priority, waits without objects, TLS.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc except where noted
// (xam/xam_info.cc). Deviations are noted case by case.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/registry.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"

namespace {

using namespace kernel;

// ExCreateThread: with this flag the returned handle is the guest KTHREAD.
constexpr uint32_t kCreateReturnKThread = 0x80;

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

void LogGuestError(const char* fn, const std::string& what) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 200)
    LogLine(std::string("[kernel] ") + fn + ": " + what + " (caller " + Hex(ExportCaller()) + ")");
}

std::shared_ptr<GuestThread> FromHandle(uint32_t handle, const char* fn) {
  auto t = std::dynamic_pointer_cast<GuestThread>(HandleGet(handle));
  if (!t) LogGuestError(fn, "unknown thread handle " + Hex(handle));
  return t;
}

std::shared_ptr<GuestThread> FromKThread(const void* kthread, const char* fn) {
  const uint32_t addr = GuestAddr(kthread);
  auto t = ThreadFromKThread(addr);
  if (!t) LogGuestError(fn, "unknown KTHREAD " + Hex(addr));
  return t;
}

// Threads created with flag 0x80 (no handle, only the KTHREAD): they stay
// alive until the process exits, because the game may wait on or
// resume the KTHREAD even after the thread has ended.
void KeepAlive(std::shared_ptr<GuestThread> t) {
  static std::mutex mutex;
  static auto* kept = new std::vector<std::shared_ptr<GuestThread>>;
  std::lock_guard g(mutex);
  kept->push_back(std::move(t));
}

// ---- creation and end ---------------------------------------------------------

// Xenia: ExCreateThread_entry. Stack 0 = the XEX header's; rounded
// to 4 KB, minimum 16 KB (GuestThread then raises it to 64 KB).
uint32_t ExCreateThreadImpl(be<uint32_t>* handle_ptr, uint32_t stack_size, be<uint32_t>* thread_id_ptr,
                            uint32_t xapi_thread_startup, uint32_t start_address, uint32_t start_context,
                            uint32_t creation_flags) {
  uint32_t actual = stack_size ? stack_size : DefaultStackSize();
  actual = std::max<uint32_t>(0x4000, (actual + 0xFFF) & 0xFFFFF000);
  auto t = GuestThread::Create(start_address, start_context, actual, (creation_flags & kCreateSuspended) != 0,
                               xapi_thread_startup, creation_flags, true);
  if (!t) {
    LogGuestError("ExCreateThread", "creation of thread " + Hex(start_address) + " failed");
    return kStatusNoMemory;
  }
  if (thread_id_ptr) *thread_id_ptr = t->ThreadId();
  if (creation_flags & kCreateReturnKThread) {
    if (handle_ptr) *handle_ptr = t->KThreadAddr();
    KeepAlive(std::move(t));
  } else {
    const uint32_t handle = HandleCreate(t);
    if (handle_ptr) *handle_ptr = handle;
  }
  return kStatusSuccess;
}

// Xenia: ExTerminateThread_entry (XThread::Exit). Does not return.
void ExTerminateThreadImpl(uint32_t exit_code) {
  GuestThread* t = GuestThread::Current();
  if (!t) {
    LogGuestError("ExTerminateThread", "called outside a guest thread");
    return;
  }
  t->Exit(exit_code);
}

// ---- suspension ----------------------------------------------------------------

// Xenia: NtResumeThread_entry. *suspend_count = previous count.
uint32_t NtResumeThreadImpl(uint32_t handle, be<uint32_t>* suspend_count) {
  auto t = FromHandle(handle, "NtResumeThread");
  if (!t) return kStatusInvalidHandle;
  const uint32_t prev = t->Resume();
  if (suspend_count) *suspend_count = prev;
  return kStatusSuccess;
}

// Xenia: NtSuspendThread_entry. Deviation: another thread stops at its
// next export instead of immediately (no host SuspendThread, which
// could stop it inside a runtime lock).
uint32_t NtSuspendThreadImpl(uint32_t handle, be<uint32_t>* suspend_count) {
  auto t = FromHandle(handle, "NtSuspendThread");
  if (!t) return kStatusInvalidHandle;
  const uint32_t prev = t->Suspend();
  if (suspend_count) *suspend_count = prev;
  return kStatusSuccess;
}

// Xenia: KeResumeThread_entry (returns a status, not NT's count).
uint32_t KeResumeThreadImpl(DispatcherHeader* kthread) {
  auto t = FromKThread(kthread, "KeResumeThread");
  if (!t) return kStatusInvalidHandle;
  t->Resume();
  return kStatusSuccess;
}

// KeSuspendThread (absent in Xenia; NT semantics): returns the previous
// count. Same deviation as NtSuspendThread.
uint32_t KeSuspendThreadImpl(DispatcherHeader* kthread) {
  auto t = FromKThread(kthread, "KeSuspendThread");
  return t ? t->Suspend() : 0;
}

// ---- current thread, affinity, priority ------------------------------------------

// KeGetCurrentThread (inline on the console, Xenia reads it from r13 + 0x100):
// the KTHREAD indicated by the current thread's PCR, 0 outside guest threads.
uint32_t KeGetCurrentThreadImpl() {
  GuestThread* t = GuestThread::Current();
  return t ? uint32_t(*GuestPtr<be<uint32_t>>(t->PcrAddr() + kPcrCurrentThread)) : 0;
}

// KeGetCurrentProcessorNumber (Xenia reads it from r13 + 0x10C): CPU recorded
// in the current thread's PCR.
uint32_t KeGetCurrentProcessorNumberImpl() {
  GuestThread* t = GuestThread::Current();
  return t ? t->Cpu() : 0;
}

// Xenia: KeSetAffinityThread_entry: the previous mask goes in
// *previous_affinity, the return value is the status.
uint32_t KeSetAffinityThreadImpl(DispatcherHeader* kthread, uint32_t affinity, be<uint32_t>* previous_affinity) {
  if (!affinity) return kStatusInvalidParameter;
  if (auto t = FromKThread(kthread, "KeSetAffinityThread")) {
    const uint32_t prev = t->SetAffinity(affinity);
    if (previous_affinity) *previous_affinity = prev;
  }
  return kStatusSuccess;
}

// Xenia: KeSetBasePriorityThread_entry: returns the previous priority.
uint32_t KeSetBasePriorityThreadImpl(DispatcherHeader* kthread, int32_t increment) {
  auto t = FromKThread(kthread, "KeSetBasePriorityThread");
  return t ? uint32_t(t->SetPriority(increment)) : 0;
}

// Xenia: KeQueryBasePriorityThread_entry. Deviation: the recorded
// increment instead of the host thread's priority.
uint32_t KeQueryBasePriorityThreadImpl(DispatcherHeader* kthread) {
  auto t = FromKThread(kthread, "KeQueryBasePriorityThread");
  return t ? uint32_t(t->Priority()) : 0;
}

// ---- waits without objects ---------------------------------------------------------

// Xenia: KeDelayExecutionThread_entry (XThread::Delay). Deviations: absolute
// intervals are handled (Xenia treats them as 0) and an alertable wait
// delivers APCs and returns STATUS_USER_APC / STATUS_ALERTED.
uint32_t KeDelayExecutionThreadImpl(uint32_t /*processor_mode*/, uint32_t alertable, be<int64_t>* interval) {
  if (!interval) {
    LogGuestError("KeDelayExecutionThread", "null interval");
    return kStatusInvalidParameter;
  }
  return DelayExecution(*interval, alertable != 0);
}

// Xenia: NtYieldExecution_entry (Delay(0, 0, 0)).
uint32_t NtYieldExecutionImpl() {
  DelayExecution(0, false);
  return kStatusSuccess;
}

// ---- TLS -------------------------------------------------------------------------

// Xenia: KeTlsAlloc_entry: global slot, zeroed in the current thread.
uint32_t KeTlsAllocImpl() {
  const uint32_t slot = GuestTlsAlloc();
  if (slot != kTlsOutOfIndexes)
    if (GuestThread* t = GuestThread::Current()) t->SetTls(slot, 0);
  return slot;
}

// Xenia: KeTlsFree_entry.
uint32_t KeTlsFreeImpl(uint32_t slot) {
  if (slot == kTlsOutOfIndexes) return 0;
  return GuestTlsFree(slot) ? 1 : 0;
}

// Xenia: KeTlsGetValue_entry (0 if the slot is out of range).
uint32_t KeTlsGetValueImpl(uint32_t slot) {
  uint32_t value = 0;
  if (GuestThread* t = GuestThread::Current()) t->GetTls(slot, &value);
  return value;
}

// Xenia: KeTlsSetValue_entry.
uint32_t KeTlsSetValueImpl(uint32_t slot, uint32_t value) {
  GuestThread* t = GuestThread::Current();
  return t && t->SetTls(slot, value) ? 1 : 0;
}

// ---- XAM ----------------------------------------------------------------------------

// Xenia: XGetAVPack_entry (xam/xam_info.cc): 6 = VGA; games use it as a
// PAL check and fail with values other than 3/4/6/8.
uint32_t XGetAVPackImpl() { return 6; }

// Xenia: KiApcNormalRoutineNop_entry (xboxkrnl_threading.cc): empty normal
// routine of the kernel APCs; returns 0.
uint32_t KiApcNormalRoutineNopImpl(uint32_t, uint32_t) { return 0; }

// ---- interlocked singly linked lists (X_SLIST_HEADER) ----------------------------

// X_SLIST_HEADER (8 bytes, big-endian): next u32, depth u16, sequence u16. The
// game pushes inline with ldarx/stdcx. on the same 8 bytes: here a 64-bit
// CAS on the raw bytes, like Xenia's xe::atomic_cas.
struct SListHeader {
  uint32_t next;
  uint16_t depth, sequence;
};
SListHeader DecodeSList(uint64_t raw) {
  uint8_t b[8];
  std::memcpy(b, &raw, 8);
  return {uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3], uint16_t(b[4] << 8 | b[5]),
          uint16_t(b[6] << 8 | b[7])};
}
uint64_t EncodeSList(SListHeader h) {
  const uint8_t b[8] = {uint8_t(h.next >> 24), uint8_t(h.next >> 16), uint8_t(h.next >> 8), uint8_t(h.next),
                        uint8_t(h.depth >> 8), uint8_t(h.depth),     uint8_t(h.sequence >> 8), uint8_t(h.sequence)};
  uint64_t raw;
  std::memcpy(&raw, b, 8);
  return raw;
}

// Xenia: InterlockedPopEntrySList_entry (xboxkrnl_threading.cc): removes the
// head (0 if empty), depth - 1, sequence unchanged.
uint32_t InterlockedPopEntrySListImpl(uint32_t list) {
  uint64_t* p = GuestPtr<uint64_t>(list);
  if (!p) return 0;
  std::atomic_ref<uint64_t> header(*p);
  uint64_t old = header.load();
  for (;;) {
    const SListHeader h = DecodeSList(old);
    if (!h.next) return 0;
    const uint32_t next = *GuestPtr<be<uint32_t>>(h.next);
    if (header.compare_exchange_weak(old, EncodeSList({next, uint16_t(h.depth - 1), h.sequence}))) return h.next;
  }
}

// Xenia: InterlockedFlushSList_entry: empties the list and returns the old
// head (header zeroed, sequence included).
uint32_t InterlockedFlushSListImpl(uint32_t list) {
  uint64_t* p = GuestPtr<uint64_t>(list);
  if (!p) return 0;
  return DecodeSList(std::atomic_ref<uint64_t>(*p).exchange(0)).next;
}

}  // namespace

KERNEL_EXPORT(ExCreateThread, ExCreateThreadImpl)
KERNEL_EXPORT(ExTerminateThread, ExTerminateThreadImpl)
KERNEL_EXPORT(NtResumeThread, NtResumeThreadImpl)
KERNEL_EXPORT(NtSuspendThread, NtSuspendThreadImpl)
KERNEL_EXPORT(KeResumeThread, KeResumeThreadImpl)
KERNEL_EXPORT(KeSuspendThread, KeSuspendThreadImpl)
KERNEL_EXPORT(KeGetCurrentThread, KeGetCurrentThreadImpl)
KERNEL_EXPORT(KeGetCurrentProcessorNumber, KeGetCurrentProcessorNumberImpl)
KERNEL_EXPORT(KeSetAffinityThread, KeSetAffinityThreadImpl)
KERNEL_EXPORT(KeSetBasePriorityThread, KeSetBasePriorityThreadImpl)
KERNEL_EXPORT(KeQueryBasePriorityThread, KeQueryBasePriorityThreadImpl)
KERNEL_EXPORT(KeDelayExecutionThread, KeDelayExecutionThreadImpl)
KERNEL_EXPORT(NtYieldExecution, NtYieldExecutionImpl)
KERNEL_EXPORT(KeTlsAlloc, KeTlsAllocImpl)
KERNEL_EXPORT(KeTlsFree, KeTlsFreeImpl)
KERNEL_EXPORT(KeTlsGetValue, KeTlsGetValueImpl)
KERNEL_EXPORT(KeTlsSetValue, KeTlsSetValueImpl)
KERNEL_EXPORT(XGetAVPack, XGetAVPackImpl)
KERNEL_EXPORT(KiApcNormalRoutineNop, KiApcNormalRoutineNopImpl)
KERNEL_EXPORT(InterlockedPopEntrySList, InterlockedPopEntrySListImpl)
KERNEL_EXPORT(InterlockedFlushSList, InterlockedFlushSListImpl)
