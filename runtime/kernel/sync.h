// HLE kernel dispatcher: single lock, any/all waits with timeout and
// alertable, timer service thread, critical sections and IRQL.
//
// Like the console's dispatcher lock, a single std::mutex protects the state
// of all waitable objects and a single std::condition_variable wakes the
// waiting threads. Whoever signals an object immediately satisfies, with the lock held, the
// waits that can now complete (SatisfyWaiters, like KiWaitTest): the
// object is consumed on behalf of the waiting thread, so there are
// no lost wakeups nor thefts between the notification and the wakeup, and WaitAll
// acquires all objects in a single step.
//
// Rule: no shared_ptr<KernelObject> may be destroyed while the dispatcher
// lock is held (the destructors of Mutant and Timer take it).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include "kernel/objects.h"

namespace kernel {

// ---- threads --------------------------------------------------------------

// Id of the current thread (never 0): owner of critical sections and mutants.
// Without a provider (or if it returns 0, host thread) it is a per-host-thread id
// (counter); with threads.cpp's provider it is the KTHREAD address.
uint32_t CurrentThreadId();
using ThreadIdProvider = uint32_t (*)();  // 0 = no id: the host one is used
void SetThreadIdProvider(ThreadIdProvider);

// ---- dispatcher lock --------------------------------------------------------

std::unique_lock<std::mutex> LockDispatcher();
// Thread on whose behalf IsSignaled/Acquire are evaluated (lock held).
uint32_t ActingThreadId();
// Satisfies, in FIFO order, the waits on `obj` that can now complete.
// To be called with the lock held after obj has become signaled.
void SatisfyWaiters(KernelObject& obj);

// ---- waits ------------------------------------------------------------------

inline constexpr size_t kMaximumWaitObjects = 64;

// timeout: nullptr = infinite; otherwise Xbox value in 100 ns: 0 = just try,
// negative = relative, positive = absolute FILETIME on the
// KeQuerySystemTime clock (rtl::QuerySystemTime).
// WaitAny: STATUS_WAIT_0 + i (lowest index among the signaled) or
// STATUS_ABANDONED_WAIT_0 + i; WaitAll: STATUS_WAIT_0 or
// STATUS_ABANDONED_WAIT_0. Both: STATUS_TIMEOUT, or, if alertable, the
// AlertPoll code (STATUS_USER_APC / STATUS_ALERTED).
uint32_t WaitAny(std::span<const std::shared_ptr<KernelObject>>, const int64_t* timeout, bool alertable);
uint32_t WaitAll(std::span<const std::shared_ptr<KernelObject>>, const int64_t* timeout, bool alertable);
// Signals `signal` (event: Set; semaphore: Release(1); mutant: Release) and
// waits on `wait`, with no window between the two. STATUS_OBJECT_TYPE_MISMATCH
// if `signal` cannot be signaled; STATUS_SEMAPHORE_LIMIT_EXCEEDED /
// STATUS_MUTANT_NOT_OWNED from the release.
uint32_t SignalAndWait(const std::shared_ptr<KernelObject>& signal, const std::shared_ptr<KernelObject>& wait,
                       const int64_t* timeout, bool alertable);

// Wait without objects (KeDelayExecutionThread): interval like the timeouts
// above (Xbox value). STATUS_SUCCESS at the end of the interval, or, if
// alertable, the AlertPoll code as soon as something is pending.
// Interval 0 (NtYieldExecution): yields the processor (std::this_thread::yield
// outside the lock) and returns STATUS_SUCCESS, unless an AlertPoll is pending.
uint32_t DelayExecution(int64_t interval, bool alertable);

// Hook for alertable waits (threads.cpp: guest thread APCs and alerts).
// Called with the dispatcher lock held, before blocking and at every
// wakeup, for the waiting thread: 0 = nothing pending, otherwise the
// status the wait ends with (STATUS_USER_APC 0xC0 / STATUS_ALERTED
// 0x101). Must not take the dispatcher lock.
using AlertPollFn = uint32_t (*)(uint32_t thread_id);
void SetAlertPoll(AlertPollFn);
// APC delivery (threads.cpp): called without the lock, on the thread that
// waited, when an alertable wait (WaitAny/WaitAll/SignalAndWait/
// DelayExecution) ends with STATUS_USER_APC; the wait then returns
// STATUS_USER_APC, like Xenia.
using ApcDeliveryFn = void (*)();
void SetApcDelivery(ApcDeliveryFn);
// Wakes all waiting threads so they re-check AlertPoll (to be
// called after queuing an APC or an alert). Takes the lock.
void WakeWaiters();

// Hook for timer APC routines (NtSetTimerEx): on every fire, without
// the dispatcher lock, with the system time (FILETIME) of the fire. Without
// a hook the routine is only logged (once).
using TimerApcFn = void (*)(uint32_t thread_id, uint32_t routine, uint32_t routine_arg, uint64_t system_time);
void SetTimerApcHook(TimerApcFn);

namespace detail {
// Timer service thread: signals that `t` has been (re)set or
// cancelled (lock held).
void TimerChanged(Timer& t);
void TimerDestroyed(Timer& t);  // lock held
int64_t SteadyNowNs();
// Absolute deadline (steady_clock, ns) of an Xbox value != nullptr;
// INT64_MAX if too far away to be represented.
int64_t DeadlineNs(int64_t xbox_time);
}  // namespace detail

// ---- critical sections ------------------------------------------------------

// The console's RTL_CRITICAL_SECTION (Xenia X_RTL_CRITICAL_SECTION,
// xboxkrnl_rtl.cc): header of a synchronization event that whoever
// finds the section busy waits on. Big-endian fields updated with host atomics.
struct CriticalSection {
  DispatcherHeader header;
  be<int32_t> lock_count;       // -1 free; +1 for each Enter
  be<int32_t> recursion_count;  // owner's Enters
  be<uint32_t> owning_thread;   // owner's CurrentThreadId(), 0 if free
};
static_assert(sizeof(CriticalSection) == 28);

void CriticalSectionInit(uint32_t guest_cs, uint32_t spin_count);
void CriticalSectionEnter(uint32_t guest_cs);
bool CriticalSectionTryEnter(uint32_t guest_cs);
void CriticalSectionLeave(uint32_t guest_cs);

// ---- IRQL, critical regions -----------------------------------------------------

// IRQL of the current thread (0 PASSIVE, 2 DISPATCH). Kept per host thread,
// not in the PCR.
uint32_t CurrentIrql();
uint32_t RaiseIrql(uint32_t irql);  // returns the previous one
void LowerIrql(uint32_t irql);
// KeEnterCriticalRegion/KeLeaveCriticalRegion (KTHREAD.KernelApcDisable).
int32_t CriticalRegionDepth();
void EnterCriticalRegion();
void LeaveCriticalRegion();

}  // namespace kernel
