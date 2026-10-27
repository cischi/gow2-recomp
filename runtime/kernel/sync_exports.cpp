// Kernel exports for objects, waits, critical sections, spinlocks and IRQL.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc except where noted
// (xboxkrnl_rtl.cc, xboxkrnl_ob.cc). Deviations are noted case by case.
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/registry.h"
#include "kernel/sync.h"
#include "log.h"

namespace {

using namespace kernel;

constexpr uint32_t kWaitAll = 0;  // WAIT_TYPE: WaitAll = 0, WaitAny = 1
constexpr uint32_t kDispatchLevel = 2;

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

// Logs guest errors with the caller; after kMaxLogged lines it goes quiet
// (a game that errs in a loop would fill the log).
void LogGuestError(const char* fn, const std::string& what) {
  static std::atomic<int> logged{0};
  constexpr int kMaxLogged = 200;
  const int n = logged.fetch_add(1);
  if (n < kMaxLogged)
    LogLine(std::string("[kernel] ") + fn + ": " + what + " (caller " + Hex(ExportCaller()) + ")");
  else if (n == kMaxLogged)
    LogLine("[kernel] further handle/object errors not logged");
}

template <class T>
std::shared_ptr<T> FromHandle(uint32_t handle, uint32_t& status, const char* fn) {
  auto obj = HandleGet(handle);
  if (!obj) {
    status = kStatusInvalidHandle;
    LogGuestError(fn, "unknown handle " + Hex(handle));
    return nullptr;
  }
  auto typed = std::dynamic_pointer_cast<T>(obj);
  if (!typed) {
    status = kStatusObjectTypeMismatch;
    LogGuestError(fn, "handle " + Hex(handle) + " of wrong type");
  }
  return typed;
}

template <class T>
std::shared_ptr<T> FromGuest(DispatcherHeader* header, const char* fn) {
  const uint32_t addr = GuestAddr(header);
  auto typed = std::dynamic_pointer_cast<T>(ObjectFromGuest(addr));
  if (!typed) LogGuestError(fn, "guest object " + Hex(addr) + " invalid");
  return typed;
}

const int64_t* Timeout(const be<int64_t>* p, int64_t& storage) {
  if (!p) return nullptr;
  storage = *p;
  return &storage;
}

// Writes the header of an object initialized by KeInitialize* and forgets
// any previous object at the same address (Review Focus 2).
void InitHeader(DispatcherHeader* h, uint8_t type, uint8_t size_words, int32_t signal_state) {
  *h = {};
  h->type = type;
  h->size = size_words;
  h->signal_state = signal_state;
  ResetGuestObject(GuestAddr(h));
}

uint32_t StoreHandle(be<uint32_t>* out, std::shared_ptr<KernelObject> obj) {
  const uint32_t h = HandleCreate(std::move(obj));
  if (out) *out = h;
  return kStatusSuccess;
}

// ---- events --------------------------------------------------------------

// Xenia: KeInitializeEvent_entry. Header zeroed, then a new object.
void KeInitializeEventImpl(DispatcherHeader* ev, uint32_t type, uint32_t state) {
  if (!ev) return;
  InitHeader(ev, uint8_t(type), sizeof(DispatcherHeader) / 4, state ? 1 : 0);
  ObjectFromGuest(GuestAddr(ev));
}

// Xenia: KeSetEvent_entry (xeKeSetEvent). Returns the previous state.
uint32_t KeSetEventImpl(DispatcherHeader* ev, uint32_t, uint32_t) {
  auto e = FromGuest<Event>(ev, "KeSetEvent");
  return e ? e->Set() : 0;
}

// Xenia: KeResetEvent_entry.
uint32_t KeResetEventImpl(DispatcherHeader* ev) {
  auto e = FromGuest<Event>(ev, "KeResetEvent");
  return e ? e->Reset() : 0;
}

// Xenia: KePulseEvent_entry.
uint32_t KePulseEventImpl(DispatcherHeader* ev, uint32_t, uint32_t) {
  auto e = FromGuest<Event>(ev, "KePulseEvent");
  return e ? e->Pulse() : 0;
}

// Xenia: NtCreateEvent_entry. event_type 0 = NotificationEvent (manual),
// 1 = SynchronizationEvent. Deviation: named objects are not
// shared (no lookup by name).
uint32_t NtCreateEventImpl(be<uint32_t>* handle_out, uint32_t, uint32_t event_type, uint32_t initial_state) {
  return StoreHandle(handle_out, std::make_shared<Event>(event_type == 0, initial_state != 0));
}

// Xenia: NtSetEvent_entry (xeNtSetEvent).
uint32_t NtSetEventImpl(uint32_t handle, be<uint32_t>* previous_state) {
  uint32_t status = kStatusSuccess;
  auto e = FromHandle<Event>(handle, status, "NtSetEvent");
  if (!e) return status;
  const int32_t prev = e->Set();
  if (previous_state) *previous_state = uint32_t(prev);
  return kStatusSuccess;
}

// Xenia: NtClearEvent_entry (xeNtClearEvent).
uint32_t NtClearEventImpl(uint32_t handle) {
  uint32_t status = kStatusSuccess;
  auto e = FromHandle<Event>(handle, status, "NtClearEvent");
  if (!e) return status;
  e->Reset();
  return kStatusSuccess;
}

// Xenia: NtPulseEvent_entry.
uint32_t NtPulseEventImpl(uint32_t handle, be<uint32_t>* previous_state) {
  uint32_t status = kStatusSuccess;
  auto e = FromHandle<Event>(handle, status, "NtPulseEvent");
  if (!e) return status;
  const int32_t prev = e->Pulse();
  if (previous_state) *previous_state = uint32_t(prev);
  return kStatusSuccess;
}

// ---- semaphores -----------------------------------------------------------

// Xenia: KeInitializeSemaphore_entry. KSEMAPHORE = header + Limit.
void KeInitializeSemaphoreImpl(DispatcherHeader* sem, int32_t count, int32_t limit) {
  if (!sem) return;
  InitHeader(sem, kSemaphoreObject, 0x14 / 4, count);
  *reinterpret_cast<be<int32_t>*>(sem + 1) = limit;
  ObjectFromGuest(GuestAddr(sem));
}

// Xenia: KeReleaseSemaphore_entry (xeKeReleaseSemaphore). Returns the
// previous count. Past the limit the console raises
// STATUS_SEMAPHORE_LIMIT_EXCEEDED: here it is logged and the count does not change.
uint32_t KeReleaseSemaphoreImpl(DispatcherHeader* sem, uint32_t, int32_t adjustment, uint32_t) {
  auto s = FromGuest<Semaphore>(sem, "KeReleaseSemaphore");
  if (!s) return 0;
  int32_t prev = 0;
  if (!s->Release(adjustment, &prev)) LogGuestError("KeReleaseSemaphore", "semaphore limit exceeded");
  return uint32_t(prev);
}

// Xenia: NtCreateSemaphore_entry (XSemaphore::Initialize rejects limit <= 0,
// count < 0 and count > limit).
uint32_t NtCreateSemaphoreImpl(be<uint32_t>* handle_out, uint32_t, int32_t count, int32_t limit) {
  if (limit <= 0 || count < 0 || count > limit) {
    if (handle_out) *handle_out = 0;
    return kStatusInvalidParameter;
  }
  return StoreHandle(handle_out, std::make_shared<Semaphore>(count, limit));
}

// Xenia: NtReleaseSemaphore_entry. Deviation: past the limit it returns
// STATUS_SEMAPHORE_LIMIT_EXCEEDED like the console.
uint32_t NtReleaseSemaphoreImpl(uint32_t handle, int32_t release_count, be<int32_t>* previous_count) {
  uint32_t status = kStatusSuccess;
  auto s = FromHandle<Semaphore>(handle, status, "NtReleaseSemaphore");
  if (!s) return status;
  int32_t prev = 0;
  const bool ok = s->Release(release_count, &prev);
  if (previous_count) *previous_count = prev;
  return ok ? kStatusSuccess : kStatusSemaphoreLimitExceeded;
}

// ---- mutants ---------------------------------------------------------------

// Xenia does not implement KeInitializeMutant (XMutant::InitializeNative is
// assert_always); NT semantics: SignalState 0 with the initial owner
// (the current thread), 1 otherwise. KMUTANT takes 0x20 bytes.
void KeInitializeMutantImpl(DispatcherHeader* mut, uint32_t initial_owner) {
  if (!mut) return;
  std::memset(mut, 0, 0x20);
  InitHeader(mut, kMutantObject, 0x20 / 4, initial_owner ? 0 : 1);
  ObjectFromGuest(GuestAddr(mut));
}

// KeReleaseMutant (absent in Xenia; NtReleaseMutant_entry -> XMutant::
// ReleaseMutant): returns the previous SignalState. Whoever does not own it
// (the console raises STATUS_MUTANT_NOT_OWNED) is logged.
uint32_t KeReleaseMutantImpl(DispatcherHeader* mut, uint32_t, uint32_t abandoned, uint32_t) {
  auto m = FromGuest<Mutant>(mut, "KeReleaseMutant");
  if (!m) return 0;
  int32_t prev = 0;
  if (m->Release(abandoned != 0, &prev) != kStatusSuccess)
    LogGuestError("KeReleaseMutant", "mutant not owned by the current thread");
  return uint32_t(prev);
}

// Xenia: NtCreateMutant_entry.
uint32_t NtCreateMutantImpl(be<uint32_t>* handle_out, uint32_t, uint32_t initial_owner) {
  return StoreHandle(handle_out, std::make_shared<Mutant>(initial_owner != 0));
}

// Xenia: NtReleaseMutant_entry. The second argument (PreviousCount in NT,
// "unknown" in Xenia, always 0 in games) receives the previous state if not null.
uint32_t NtReleaseMutantImpl(uint32_t handle, be<int32_t>* previous_count) {
  uint32_t status = kStatusSuccess;
  auto m = FromHandle<Mutant>(handle, status, "NtReleaseMutant");
  if (!m) return status;
  int32_t prev = 0;
  status = m->Release(false, &prev);
  if (previous_count && status == kStatusSuccess) *previous_count = prev;
  return status;
}

// ---- timers -------------------------------------------------------------------

// KeInitializeTimerEx (absent in Xenia; NT semantics): 0x28-byte KTIMER,
// Type 8 + TimerType (0 notification, 1 synchronization).
void KeInitializeTimerExImpl(DispatcherHeader* timer, uint32_t type) {
  if (!timer) return;
  std::memset(timer, 0, 0x28);
  InitHeader(timer, uint8_t(kNotificationTimer + (type ? 1 : 0)), 0x28 / 4, 0);
  ObjectFromGuest(GuestAddr(timer));
}

// KeSetTimerEx (absent in Xenia; XTimer::SetTimer for the semantics):
// DueTime by value (r4, 64 bit), Period in ms. Returns TRUE if the timer was
// already active. Deviation: the DPC is not executed (logged once).
uint32_t KeSetTimerExImpl(DispatcherHeader* timer, int64_t due_time, int32_t period, uint32_t dpc) {
  auto t = FromGuest<Timer>(timer, "KeSetTimerEx");
  if (!t) return 0;
  if (dpc) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) LogGuestError("KeSetTimerEx", "DPC " + Hex(dpc) + " not executed");
  }
  return t->Set(due_time, uint32_t(period < 0 ? 0 : period), 0, 0);
}

// KeCancelTimer (absent in Xenia): TRUE if the timer was active.
uint32_t KeCancelTimerImpl(DispatcherHeader* timer) {
  auto t = FromGuest<Timer>(timer, "KeCancelTimer");
  return t ? t->Cancel() : 0;
}

// Xenia: NtCreateTimer_entry (timer_type 0 notification, 1 synchronization).
uint32_t NtCreateTimerImpl(be<uint32_t>* handle_out, uint32_t, uint32_t timer_type) {
  return StoreHandle(handle_out, std::make_shared<Timer>(timer_type != 0));
}

// Xenia: NtSetTimerEx_entry. Deviation: with resume the timer is set
// anyway and returns STATUS_TIMER_RESUME_IGNORED (informational, like NT), where
// Xenia exits without setting it.
uint32_t NtSetTimerExImpl(uint32_t handle, be<int64_t>* due_time, uint32_t routine, uint32_t, uint32_t routine_arg,
                          uint32_t resume, uint32_t period_ms, uint32_t) {
  uint32_t status = kStatusSuccess;
  auto t = FromHandle<Timer>(handle, status, "NtSetTimerEx");
  if (!t) return status;
  if (!due_time) return kStatusInvalidParameter;
  t->Set(*due_time, period_ms, routine, routine_arg);
  return resume ? kStatusTimerResumeIgnored : kStatusSuccess;
}

// Xenia: NtCancelTimer_entry. current_state receives the signaled state.
uint32_t NtCancelTimerImpl(uint32_t handle, be<uint32_t>* current_state) {
  uint32_t status = kStatusSuccess;
  auto t = FromHandle<Timer>(handle, status, "NtCancelTimer");
  if (!t) return status;
  t->Cancel();
  if (current_state) *current_state = t->SignalState() ? 1 : 0;
  return kStatusSuccess;
}

// ---- waits -----------------------------------------------------------------------

// Xenia: KeWaitForSingleObject_entry (xeKeWaitForSingleObject). Deviation:
// an unrecognized object gives STATUS_INVALID_PARAMETER (Xenia
// STATUS_ABANDONED_WAIT_0).
uint32_t KeWaitForSingleObjectImpl(DispatcherHeader* object, uint32_t, uint32_t, uint32_t alertable,
                                   be<int64_t>* timeout_ptr) {
  auto obj = ObjectFromGuest(GuestAddr(object));
  if (!obj) {
    LogGuestError("KeWaitForSingleObject", "guest object " + Hex(GuestAddr(object)) + " not waitable");
    return kStatusInvalidParameter;
  }
  int64_t timeout;
  const std::shared_ptr<KernelObject> one[1] = {std::move(obj)};
  return WaitAny(one, Timeout(timeout_ptr, timeout), alertable != 0);
}

// Xenia: NtWaitForSingleObjectEx_entry.
uint32_t NtWaitForSingleObjectExImpl(uint32_t handle, uint32_t, uint32_t alertable, be<int64_t>* timeout_ptr) {
  auto obj = HandleGet(handle);
  if (!obj) {
    LogGuestError("NtWaitForSingleObjectEx", "unknown handle " + Hex(handle));
    return kStatusInvalidHandle;
  }
  int64_t timeout;
  const std::shared_ptr<KernelObject> one[1] = {std::move(obj)};
  return WaitAny(one, Timeout(timeout_ptr, timeout), alertable != 0);
}

// Xenia: KeWaitForMultipleObjects_entry. wait_type 0 = WaitAll, 1 = WaitAny.
uint32_t KeWaitForMultipleObjectsImpl(uint32_t count, be<uint32_t>* objects, uint32_t wait_type, uint32_t, uint32_t,
                                      uint32_t alertable, be<int64_t>* timeout_ptr, uint32_t) {
  if (!objects || count == 0 || count > kMaximumWaitObjects) return kStatusInvalidParameter;
  std::vector<std::shared_ptr<KernelObject>> list;
  list.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto obj = ObjectFromGuest(objects[i]);
    if (!obj) {
      LogGuestError("KeWaitForMultipleObjects", "guest object " + Hex(objects[i]) + " not waitable");
      return kStatusInvalidParameter;
    }
    list.push_back(std::move(obj));
  }
  int64_t timeout;
  const int64_t* t = Timeout(timeout_ptr, timeout);
  return wait_type == kWaitAll ? WaitAll(list, t, alertable != 0) : WaitAny(list, t, alertable != 0);
}

// Xenia: NtWaitForMultipleObjectsEx_entry (xeNtWaitForMultipleObjectsEx).
// Deviation: an unknown handle gives STATUS_INVALID_HANDLE like NT (Xenia
// STATUS_INVALID_PARAMETER).
uint32_t NtWaitForMultipleObjectsExImpl(uint32_t count, be<uint32_t>* handles, uint32_t wait_type, uint32_t,
                                        uint32_t alertable, be<int64_t>* timeout_ptr) {
  if (!handles || count == 0 || count > kMaximumWaitObjects) return kStatusInvalidParameter;
  std::vector<std::shared_ptr<KernelObject>> list;
  list.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto obj = HandleGet(handles[i]);
    if (!obj) {
      LogGuestError("NtWaitForMultipleObjectsEx", "unknown handle " + Hex(handles[i]));
      return kStatusInvalidHandle;
    }
    list.push_back(std::move(obj));
  }
  int64_t timeout;
  const int64_t* t = Timeout(timeout_ptr, timeout);
  return wait_type == kWaitAll ? WaitAll(list, t, alertable != 0) : WaitAny(list, t, alertable != 0);
}

// Xenia: NtSignalAndWaitForSingleObjectEx_entry (XObject::SignalAndWait).
uint32_t NtSignalAndWaitForSingleObjectExImpl(uint32_t signal_handle, uint32_t wait_handle, uint32_t alertable,
                                              uint32_t, be<int64_t>* timeout_ptr) {
  auto signal = HandleGet(signal_handle);
  auto wait = HandleGet(wait_handle);
  if (!signal || !wait) {
    LogGuestError("NtSignalAndWaitForSingleObjectEx",
                  "unknown handle " + Hex(signal ? wait_handle : signal_handle));
    return kStatusInvalidHandle;
  }
  int64_t timeout;
  return SignalAndWait(signal, wait, Timeout(timeout_ptr, timeout), alertable != 0);
}

// ---- handles and references -----------------------------------------------------

// Xenia: NtClose_entry (xboxkrnl_ob.cc).
uint32_t NtCloseImpl(uint32_t handle) {
  if (HandleClose(handle)) return kStatusSuccess;
  LogGuestError("NtClose", "unknown handle " + Hex(handle));
  return kStatusInvalidHandle;
}

// Xenia: NtDuplicateObject_entry (xboxkrnl_ob.cc): new handle to the same
// object (also from the current thread's pseudo-handle 0xFFFFFFFE);
// new_handle may be null; options 1 (DUPLICATE_CLOSE_SOURCE) closes
// the source handle.
uint32_t NtDuplicateObjectImpl(uint32_t handle, be<uint32_t>* new_handle, uint32_t options) {
  auto obj = HandleGet(handle);
  uint32_t dup = 0xFFFFFFFF;  // X_INVALID_HANDLE_VALUE
  if (obj) dup = HandleCreate(obj);
  if (new_handle) *new_handle = dup;
  if (options == 1 && handle != kCurrentThreadHandle) HandleClose(handle);
  if (!obj) {
    LogGuestError("NtDuplicateObject", "unknown handle " + Hex(handle));
    return kStatusInvalidHandle;
  }
  return kStatusSuccess;
}

// Xenia: ObReferenceObjectByHandle_entry (xboxkrnl_ob.cc): returns the
// object's guest structure (created if missing) and holds a reference until
// ObDereferenceObject. Deviation: object_type is not checked (the
// Ex*ObjectType types are import variables with no content).
uint32_t ObReferenceObjectByHandleImpl(uint32_t handle, uint32_t, be<uint32_t>* out_object) {
  auto obj = HandleGet(handle);
  if (!obj) {
    LogGuestError("ObReferenceObjectByHandle", "unknown handle " + Hex(handle));
    return kStatusInvalidHandle;
  }
  const uint32_t body = EnsureGuestObject(obj);
  if (!body) return kStatusUnsuccessful;
  ObjectReference(body, std::move(obj));
  if (out_object) *out_object = body;
  return kStatusSuccess;
}

// Xenia: ObDereferenceObject_entry (xboxkrnl_ob.cc).
void ObDereferenceObjectImpl(uint32_t object) {
  if (!ObjectDereference(object))
    LogGuestError("ObDereferenceObject", "object " + Hex(object) + " has no references");
}

// ---- critical sections (xboxkrnl_rtl.cc) ------------------------------------------

// Xenia: RtlInitializeCriticalSection_entry.
void RtlInitializeCriticalSectionImpl(CriticalSection* cs) { CriticalSectionInit(GuestAddr(cs), 0); }

// Xenia: RtlInitializeCriticalSectionAndSpinCount_entry.
uint32_t RtlInitializeCriticalSectionAndSpinCountImpl(CriticalSection* cs, uint32_t spin_count) {
  CriticalSectionInit(GuestAddr(cs), spin_count);
  return kStatusSuccess;
}

// Xenia: RtlEnterCriticalSection_entry.
void RtlEnterCriticalSectionImpl(CriticalSection* cs) { CriticalSectionEnter(GuestAddr(cs)); }

// Xenia: RtlTryEnterCriticalSection_entry.
uint32_t RtlTryEnterCriticalSectionImpl(CriticalSection* cs) { return CriticalSectionTryEnter(GuestAddr(cs)); }

// Xenia: RtlLeaveCriticalSection_entry.
void RtlLeaveCriticalSectionImpl(CriticalSection* cs) { CriticalSectionLeave(GuestAddr(cs)); }

// ---- critical regions, IRQL, spinlocks ---------------------------------------------

// Xenia: KeEnterCriticalRegion_entry / KeLeaveCriticalRegion_entry.
void KeEnterCriticalRegionImpl() { EnterCriticalRegion(); }
void KeLeaveCriticalRegionImpl() { LeaveCriticalRegion(); }

// Xenia: KeRaiseIrqlToDpcLevel_entry.
uint32_t KeRaiseIrqlToDpcLevelImpl() { return RaiseIrql(kDispatchLevel); }

// Xenia: KfLowerIrql_entry. Pending APCs are not checked here.
void KfLowerIrqlImpl(uint32_t old_irql) { LowerIrql(old_irql); }

void SpinAcquire(be<uint32_t>* lock) {
  std::atomic_ref<uint32_t> ref(*reinterpret_cast<uint32_t*>(lock));
  for (uint32_t spins = 0;; ++spins) {
    uint32_t expected = 0;
    if (ref.compare_exchange_weak(expected, std::byteswap(1u))) return;
    if (spins > 64) std::this_thread::yield();
  }
}

void SpinRelease(be<uint32_t>* lock) { std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(lock)).store(0); }

// Xenia: KfAcquireSpinLock_entry (xeKeKfAcquireSpinLock): lock 0 -> 1, IRQL to
// DISPATCH; returns the previous IRQL.
uint32_t KfAcquireSpinLockImpl(be<uint32_t>* lock) {
  if (!lock) return CurrentIrql();
  SpinAcquire(lock);
  return RaiseIrql(kDispatchLevel);
}

// Xenia: KfReleaseSpinLock_entry (xeKeKfReleaseSpinLock).
void KfReleaseSpinLockImpl(be<uint32_t>* lock, uint32_t old_irql) {
  LowerIrql(old_irql);
  if (lock) SpinRelease(lock);
}

// Xenia: KeAcquireSpinLockAtRaisedIrql_entry.
void KeAcquireSpinLockAtRaisedIrqlImpl(be<uint32_t>* lock) {
  if (lock) SpinAcquire(lock);
}

// Xenia: KeReleaseSpinLockFromRaisedIrql_entry.
void KeReleaseSpinLockFromRaisedIrqlImpl(be<uint32_t>* lock) {
  if (lock) SpinRelease(lock);
}

}  // namespace

KERNEL_EXPORT(KeInitializeEvent, KeInitializeEventImpl)
KERNEL_EXPORT(KeSetEvent, KeSetEventImpl)
KERNEL_EXPORT(KeResetEvent, KeResetEventImpl)
KERNEL_EXPORT(KePulseEvent, KePulseEventImpl)
KERNEL_EXPORT(NtCreateEvent, NtCreateEventImpl)
KERNEL_EXPORT(NtSetEvent, NtSetEventImpl)
KERNEL_EXPORT(NtClearEvent, NtClearEventImpl)
KERNEL_EXPORT(NtPulseEvent, NtPulseEventImpl)
KERNEL_EXPORT(KeInitializeSemaphore, KeInitializeSemaphoreImpl)
KERNEL_EXPORT(KeReleaseSemaphore, KeReleaseSemaphoreImpl)
KERNEL_EXPORT(NtCreateSemaphore, NtCreateSemaphoreImpl)
KERNEL_EXPORT(NtReleaseSemaphore, NtReleaseSemaphoreImpl)
KERNEL_EXPORT(KeInitializeMutant, KeInitializeMutantImpl)
KERNEL_EXPORT(KeReleaseMutant, KeReleaseMutantImpl)
KERNEL_EXPORT(NtCreateMutant, NtCreateMutantImpl)
KERNEL_EXPORT(NtReleaseMutant, NtReleaseMutantImpl)
KERNEL_EXPORT(KeInitializeTimerEx, KeInitializeTimerExImpl)
KERNEL_EXPORT(KeSetTimerEx, KeSetTimerExImpl)
KERNEL_EXPORT(KeCancelTimer, KeCancelTimerImpl)
KERNEL_EXPORT(NtCreateTimer, NtCreateTimerImpl)
KERNEL_EXPORT(NtSetTimerEx, NtSetTimerExImpl)
KERNEL_EXPORT(NtCancelTimer, NtCancelTimerImpl)
KERNEL_EXPORT(KeWaitForSingleObject, KeWaitForSingleObjectImpl)
KERNEL_EXPORT(NtWaitForSingleObjectEx, NtWaitForSingleObjectExImpl)
KERNEL_EXPORT(KeWaitForMultipleObjects, KeWaitForMultipleObjectsImpl)
KERNEL_EXPORT(NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsExImpl)
KERNEL_EXPORT(NtSignalAndWaitForSingleObjectEx, NtSignalAndWaitForSingleObjectExImpl)
KERNEL_EXPORT(NtClose, NtCloseImpl)
KERNEL_EXPORT(NtDuplicateObject, NtDuplicateObjectImpl)
KERNEL_EXPORT(ObReferenceObjectByHandle, ObReferenceObjectByHandleImpl)
KERNEL_EXPORT(ObDereferenceObject, ObDereferenceObjectImpl)
KERNEL_EXPORT(RtlInitializeCriticalSection, RtlInitializeCriticalSectionImpl)
KERNEL_EXPORT(RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCountImpl)
KERNEL_EXPORT(RtlEnterCriticalSection, RtlEnterCriticalSectionImpl)
KERNEL_EXPORT(RtlTryEnterCriticalSection, RtlTryEnterCriticalSectionImpl)
KERNEL_EXPORT(RtlLeaveCriticalSection, RtlLeaveCriticalSectionImpl)
KERNEL_EXPORT(KeEnterCriticalRegion, KeEnterCriticalRegionImpl)
KERNEL_EXPORT(KeLeaveCriticalRegion, KeLeaveCriticalRegionImpl)
KERNEL_EXPORT(KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevelImpl)
KERNEL_EXPORT(KfLowerIrql, KfLowerIrqlImpl)
KERNEL_EXPORT(KfAcquireSpinLock, KfAcquireSpinLockImpl)
KERNEL_EXPORT(KfReleaseSpinLock, KfReleaseSpinLockImpl)
KERNEL_EXPORT(KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrqlImpl)
KERNEL_EXPORT(KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrqlImpl)
