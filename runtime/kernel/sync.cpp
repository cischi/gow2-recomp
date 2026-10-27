// HLE kernel dispatcher: any/all waits, timeouts, alerts, timers, critical
// sections, IRQL.
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/kernel/xobject.cc (Wait, WaitMultiple, SignalAndWait),
// xtimer.cc, xboxkrnl/xboxkrnl_rtl.cc (RtlEnterCriticalSection & co.),
// xboxkrnl/xboxkrnl_threading.cc (IRQL). The waits are built on a global lock
// and condition variable instead of the Windows primitives.
#include "kernel/sync.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <set>
#include <thread>
#include <vector>

#include "kernel/guest.h"
#include "kernel/rtl_exports.h"
#include "log.h"

namespace kernel {
namespace {

using detail::DispatcherAccess;
using detail::Waiter;

struct PendingApc {
  uint32_t thread, routine, arg;
  uint64_t time;
};

// Dispatcher state, never destroyed (objects and timer threads may
// survive until the process exits).
struct Dispatcher {
  std::mutex mutex;
  std::condition_variable cv;
  uint32_t acting_thread = 0;  // != 0 while satisfying another thread's wait
  std::atomic<AlertPollFn> alert_poll{nullptr};
  std::atomic<TimerApcFn> timer_apc{nullptr};
  std::atomic<ApcDeliveryFn> apc_delivery{nullptr};
  std::set<Timer*> active_timers;
  bool timer_thread_started = false;
};

Dispatcher& D() {
  static Dispatcher* d = new Dispatcher;
  return *d;
}

std::atomic<ThreadIdProvider> g_thread_id_provider{nullptr};
std::atomic<uint32_t> g_next_host_thread_id{1};
thread_local uint32_t t_host_thread_id = 0;
thread_local uint32_t t_irql = 0;
thread_local int32_t t_critical_region = 0;

// ---- satisfying waits (KiWaitTest) ----------------------------------------------

void AddWaiter(Waiter& w) {
  for (size_t i = 0; i < w.objects.size(); ++i) {
    auto& list = DispatcherAccess::Waiters(*w.objects[i]);
    if (std::find(list.begin(), list.end(), &w) == list.end()) list.push_back(&w);
  }
}

void RemoveWaiter(Waiter& w) {
  for (const auto& o : w.objects) {
    auto& list = DispatcherAccess::Waiters(*o);
    list.erase(std::remove(list.begin(), list.end(), &w), list.end());
  }
}

// If w's wait can complete now, acquires the objects on behalf
// of w.thread, writes w.status and returns true. Lock held.
bool TryAcquire(Waiter& w) {
  Dispatcher& d = D();
  d.acting_thread = w.thread;
  bool ok = false;
  if (!w.wait_all) {
    for (size_t i = 0; i < w.objects.size(); ++i) {
      KernelObject& o = *w.objects[i];
      if (!o.IsSignaled()) continue;
      const bool abandoned = o.IsAbandoned();
      o.Acquire();
      w.status = (abandoned ? kStatusAbandonedWait0 : kStatusWait0) + uint32_t(i);
      ok = true;
      break;
    }
  } else {
    ok = std::all_of(w.objects.begin(), w.objects.end(), [](const auto& o) { return o->IsSignaled(); });
    if (ok) {
      bool abandoned = false;
      for (const auto& o : w.objects) {
        abandoned |= o->IsAbandoned();
        o->Acquire();
      }
      w.status = abandoned ? kStatusAbandonedWait0 : kStatusWait0;
    }
  }
  d.acting_thread = 0;
  return ok;
}

// ---- time --------------------------------------------------------------------

constexpr int64_t kInfinite = INT64_MAX;
// Beyond ~100 years a timeout means infinite (and does not overflow in ns).
constexpr uint64_t kMaxTicks = 100ull * 365 * 24 * 3600 * 10'000'000;

std::chrono::steady_clock::time_point ToTimePoint(int64_t ns) {
  return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

uint32_t PollAlert(uint32_t thread) {
  const AlertPollFn poll = D().alert_poll.load();
  return poll ? poll(thread) : 0;
}

// End of an alertable wait interrupted by an APC: delivery (without the lock).
uint32_t AfterWait(uint32_t status) {
  if (status == kStatusUserApc)
    if (const ApcDeliveryFn deliver = D().apc_delivery.load()) deliver();
  return status;
}

// Wait with the lock held. deadline in steady_clock ns (kInfinite = none).
uint32_t WaitLocked(std::unique_lock<std::mutex>& lock, Waiter& w, int64_t deadline, bool alertable) {
  Dispatcher& d = D();
  if (TryAcquire(w)) return w.status;
  if (alertable)
    if (const uint32_t s = PollAlert(w.thread)) return s;
  if (deadline != kInfinite && detail::SteadyNowNs() >= deadline) return kStatusTimeout;

  AddWaiter(w);
  for (;;) {
    if (deadline == kInfinite)
      d.cv.wait(lock);
    else
      d.cv.wait_until(lock, ToTimePoint(deadline));
    if (w.done) return w.status;  // satisfied by whoever signaled
    if (alertable)
      if (const uint32_t s = PollAlert(w.thread)) {
        RemoveWaiter(w);
        return s;
      }
    if (deadline != kInfinite && detail::SteadyNowNs() >= deadline) {
      RemoveWaiter(w);
      return kStatusTimeout;
    }
  }
}

uint32_t Wait(std::span<const std::shared_ptr<KernelObject>> objects, bool wait_all, const int64_t* timeout,
              bool alertable) {
  if (objects.empty() || objects.size() > kMaximumWaitObjects) return kStatusInvalidParameter;
  for (size_t i = 0; i < objects.size(); ++i) {
    if (!objects[i]) return kStatusInvalidParameter;
    // WaitAll on the same object twice: rejected as in NT
    // (STATUS_INVALID_PARAMETER_MIX), here STATUS_INVALID_PARAMETER.
    if (wait_all)
      for (size_t j = 0; j < i; ++j)
        if (objects[j] == objects[i]) return kStatusInvalidParameter;
  }
  const int64_t deadline = timeout ? detail::DeadlineNs(*timeout) : kInfinite;
  Waiter w{objects, wait_all, CurrentThreadId()};
  uint32_t status;
  {
    auto lock = LockDispatcher();
    status = WaitLocked(lock, w, deadline, alertable);
  }
  return AfterWait(status);
}

// ---- timer service thread -----------------------------------------------------

void TimerThread() {
  Dispatcher& d = D();
  std::unique_lock lock(d.mutex);
  std::vector<PendingApc> apcs;
  for (;;) {
    const int64_t now = detail::SteadyNowNs();
    int64_t next = kInfinite;
    for (auto it = d.active_timers.begin(); it != d.active_timers.end();) {
      Timer* t = *it;
      auto st = DispatcherAccess::State(*t);
      if (st.due_ns <= now) {
        st.signaled = true;
        if (st.routine) apcs.push_back({st.apc_thread, st.routine, st.routine_arg, 0});
        if (st.period_ns > 0) {
          st.due_ns += st.period_ns;
          if (st.due_ns <= now) st.due_ns = now + st.period_ns;  // missed fires: no burst
          ++it;
        } else {
          st.active = false;
          it = d.active_timers.erase(it);
        }
        SatisfyWaiters(*t);
      } else {
        ++it;
      }
    }
    for (Timer* t : d.active_timers) next = std::min(next, DispatcherAccess::State(*t).due_ns);

    if (!apcs.empty()) {
      // APC routine outside the lock: the hook (threads.cpp) takes its own locks
      std::vector<PendingApc> todo;
      todo.swap(apcs);
      lock.unlock();
      const uint64_t time = rtl::QuerySystemTime();
      const TimerApcFn hook = d.timer_apc.load();
      for (auto& a : todo) {
        if (hook) {
          hook(a.thread, a.routine, a.arg, time);
        } else {
          static std::atomic<bool> logged{false};
          if (!logged.exchange(true))
            LogLine("[kernel] timer with an APC routine but no APC handler: routine ignored");
        }
      }
      lock.lock();
      continue;
    }
    if (next == kInfinite)
      d.cv.wait(lock);
    else
      d.cv.wait_until(lock, ToTimePoint(next));
  }
}

}  // namespace

// ---- threads -----------------------------------------------------------------

uint32_t CurrentThreadId() {
  if (const ThreadIdProvider p = g_thread_id_provider.load())
    if (const uint32_t id = p()) return id;
  if (!t_host_thread_id) {
    // host ids: never 0; they start at 1 and are not confused with a guest KTHREAD
    // (addresses >= 0x10000) as long as host threads stay below 65535.
    t_host_thread_id = g_next_host_thread_id.fetch_add(1);
    if (!t_host_thread_id) t_host_thread_id = g_next_host_thread_id.fetch_add(1);
  }
  return t_host_thread_id;
}

void SetThreadIdProvider(ThreadIdProvider p) { g_thread_id_provider = p; }

// ---- dispatcher lock -----------------------------------------------------------

std::unique_lock<std::mutex> LockDispatcher() { return std::unique_lock(D().mutex); }

uint32_t ActingThreadId() {
  const uint32_t acting = D().acting_thread;
  return acting ? acting : CurrentThreadId();
}

void SatisfyWaiters(KernelObject& obj) {
  auto& list = DispatcherAccess::Waiters(obj);
  bool woke = false;
  for (size_t i = 0; i < list.size();) {
    Waiter* w = list[i];
    if (!TryAcquire(*w)) {
      ++i;
      continue;
    }
    RemoveWaiter(*w);  // also removes w from list: list[i] is already the next one
    w->done = true;
    woke = true;
  }
  if (woke) D().cv.notify_all();
}

// ---- waits ---------------------------------------------------------------------

uint32_t WaitAny(std::span<const std::shared_ptr<KernelObject>> objects, const int64_t* timeout, bool alertable) {
  return Wait(objects, false, timeout, alertable);
}

uint32_t WaitAll(std::span<const std::shared_ptr<KernelObject>> objects, const int64_t* timeout, bool alertable) {
  return Wait(objects, true, timeout, alertable);
}

uint32_t SignalAndWait(const std::shared_ptr<KernelObject>& signal, const std::shared_ptr<KernelObject>& wait,
                       const int64_t* timeout, bool alertable) {
  if (!signal || !wait) return kStatusInvalidHandle;
  const int64_t deadline = timeout ? detail::DeadlineNs(*timeout) : kInfinite;
  const std::shared_ptr<KernelObject> objects[1] = {wait};
  Waiter w{objects, false, CurrentThreadId()};
  uint32_t status;
  {
    auto lock = LockDispatcher();
    if (auto* ev = dynamic_cast<Event*>(signal.get())) {
      ev->SetLocked();
    } else if (auto* sem = dynamic_cast<Semaphore*>(signal.get())) {
      if (!sem->ReleaseLocked(1, nullptr)) return kStatusSemaphoreLimitExceeded;
    } else if (auto* mut = dynamic_cast<Mutant*>(signal.get())) {
      if (const uint32_t s = mut->ReleaseLocked(false, nullptr)) return s;
    } else {
      return kStatusObjectTypeMismatch;
    }
    status = WaitLocked(lock, w, deadline, alertable);
  }
  return AfterWait(status);
}

uint32_t DelayExecution(int64_t interval, bool alertable) {
  const uint32_t me = CurrentThreadId();
  Dispatcher& d = D();
  if (interval == 0) {
    // Sleep(0) / NtYieldExecution: no wait, the processor is yielded.
    uint32_t status = kStatusSuccess;
    if (alertable) {
      auto lock = LockDispatcher();
      status = PollAlert(me);
    }
    if (status) return AfterWait(status);
    std::this_thread::yield();
    return kStatusSuccess;
  }
  const int64_t deadline = detail::DeadlineNs(interval);
  uint32_t status = kStatusSuccess;
  {
    auto lock = LockDispatcher();
    for (;;) {
      if (alertable)
        if ((status = PollAlert(me)) != 0) break;
      if (deadline != kInfinite && detail::SteadyNowNs() >= deadline) break;
      if (deadline == kInfinite)
        d.cv.wait(lock);
      else
        d.cv.wait_until(lock, ToTimePoint(deadline));
    }
  }
  return AfterWait(status);
}

void SetAlertPoll(AlertPollFn fn) { D().alert_poll = fn; }

void SetApcDelivery(ApcDeliveryFn fn) { D().apc_delivery = fn; }

void WakeWaiters() {
  auto lock = LockDispatcher();
  D().cv.notify_all();
}

void SetTimerApcHook(TimerApcFn fn) { D().timer_apc = fn; }

namespace detail {

void TimerChanged(Timer& t) {
  Dispatcher& d = D();
  if (DispatcherAccess::State(t).active)
    d.active_timers.insert(&t);
  else
    d.active_timers.erase(&t);
  if (!d.timer_thread_started) {
    d.timer_thread_started = true;
    std::thread(TimerThread).detach();  // lives as long as the process
  }
  d.cv.notify_all();
}

void TimerDestroyed(Timer& t) { D().active_timers.erase(&t); }

int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Xenia XObject::TimeoutTicksToMs reduces the timeout to milliseconds and does not
// handle absolute ones; here: relative in 100 ns, absolute compared with
// the KeQuerySystemTime clock and converted to a deadline on
// steady_clock, which is unaffected by clock changes.
int64_t DeadlineNs(int64_t xbox_time) {
  const int64_t now = SteadyNowNs();
  uint64_t ticks;
  if (xbox_time == 0) return now;
  if (xbox_time < 0) {
    ticks = uint64_t(0) - uint64_t(xbox_time);  // also INT64_MIN
  } else {
    const int64_t system_now = int64_t(rtl::QuerySystemTime());
    if (xbox_time <= system_now) return now;
    ticks = uint64_t(xbox_time - system_now);
  }
  if (ticks > kMaxTicks) return kInfinite;
  return now + int64_t(ticks) * 100;
}

}  // namespace detail

// ---- critical sections (Xenia xboxkrnl_rtl.cc) -------------------------------------
//
// The fields are big-endian in guest memory: the atomics work on the raw
// 32-bit value and convert it at every step.

namespace {

std::atomic_ref<uint32_t> Raw(auto& be_field) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(&be_field)); }

uint32_t Swap(uint32_t v) { return std::byteswap(v); }

int32_t LoadI32(auto& field) { return int32_t(Swap(Raw(field).load())); }
void StoreU32(auto& field, uint32_t v) { Raw(field).store(Swap(v)); }

// Atomic add; returns the new value.
int32_t AddI32(auto& field, int32_t delta) {
  auto ref = Raw(field);
  uint32_t old = ref.load();
  uint32_t neu;
  do {
    neu = Swap(uint32_t(int32_t(Swap(old)) + delta));
  } while (!ref.compare_exchange_weak(old, neu));
  return int32_t(Swap(neu));
}

bool CasI32(auto& field, int32_t expected, int32_t desired) {
  uint32_t exp = Swap(uint32_t(expected));
  return Raw(field).compare_exchange_strong(exp, Swap(uint32_t(desired)));
}

void WaitCriticalSectionEvent(uint32_t guest_cs) {
  auto ev = ObjectFromGuest(guest_cs);
  if (!ev) return;
  const std::shared_ptr<KernelObject> one[1] = {ev};
  WaitAny(one, nullptr, false);
}

void SignalCriticalSectionEvent(uint32_t guest_cs) {
  auto obj = ObjectFromGuest(guest_cs);
  if (auto* ev = dynamic_cast<Event*>(obj.get())) ev->Set();
}

}  // namespace

void CriticalSectionInit(uint32_t guest_cs, uint32_t spin_count) {
  auto* cs = GuestPtr<CriticalSection>(guest_cs);
  if (!cs) return;
  const uint32_t spin_div_256 = std::min<uint32_t>((uint64_t(spin_count) + 255) >> 8, 255);
  cs->header = {};
  cs->header.type = kSynchronizationEvent;
  cs->header.absolute = uint8_t(spin_div_256);
  cs->header.size = sizeof(DispatcherHeader) / 4;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;
  ResetGuestObject(guest_cs);
}

void CriticalSectionEnter(uint32_t guest_cs) {
  auto* cs = GuestPtr<CriticalSection>(guest_cs);
  if (!cs) return;
  const uint32_t me = CurrentThreadId();
  if (uint32_t(LoadI32(cs->owning_thread)) == me) {
    AddI32(cs->lock_count, 1);
    StoreU32(cs->recursion_count, uint32_t(LoadI32(cs->recursion_count) + 1));
    return;
  }
  for (uint32_t spin = uint32_t(cs->header.absolute) * 256; spin; --spin) {
    if (CasI32(cs->lock_count, -1, 0)) {
      StoreU32(cs->owning_thread, me);
      StoreU32(cs->recursion_count, 1);
      return;
    }
  }
  if (AddI32(cs->lock_count, 1) != 0) WaitCriticalSectionEvent(guest_cs);
  StoreU32(cs->owning_thread, me);
  StoreU32(cs->recursion_count, 1);
}

bool CriticalSectionTryEnter(uint32_t guest_cs) {
  auto* cs = GuestPtr<CriticalSection>(guest_cs);
  if (!cs) return false;
  const uint32_t me = CurrentThreadId();
  if (CasI32(cs->lock_count, -1, 0)) {
    StoreU32(cs->owning_thread, me);
    StoreU32(cs->recursion_count, 1);
    return true;
  }
  if (uint32_t(LoadI32(cs->owning_thread)) == me) {
    AddI32(cs->lock_count, 1);
    StoreU32(cs->recursion_count, uint32_t(LoadI32(cs->recursion_count) + 1));
    return true;
  }
  return false;
}

void CriticalSectionLeave(uint32_t guest_cs) {
  auto* cs = GuestPtr<CriticalSection>(guest_cs);
  if (!cs) return;
  const int32_t recursion = LoadI32(cs->recursion_count) - 1;
  StoreU32(cs->recursion_count, uint32_t(recursion));
  if (recursion > 0) {
    AddI32(cs->lock_count, -1);
    return;
  }
  StoreU32(cs->owning_thread, 0);
  // others queued: wakes one (the header's auto-reset event)
  if (AddI32(cs->lock_count, -1) != -1) SignalCriticalSectionEvent(guest_cs);
}

// ---- IRQL and critical regions ----------------------------------------------------

uint32_t CurrentIrql() { return t_irql; }

uint32_t RaiseIrql(uint32_t irql) {
  const uint32_t old = t_irql;
  if (irql > old) t_irql = irql;
  return old;
}

void LowerIrql(uint32_t irql) { t_irql = irql; }

int32_t CriticalRegionDepth() { return t_critical_region; }
void EnterCriticalRegion() { ++t_critical_region; }
void LeaveCriticalRegion() { --t_critical_region; }

}  // namespace kernel
