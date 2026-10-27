// Kernel objects and synchronization (runtime/kernel/objects.*, sync.*,
// sync_exports.cpp): events, semaphores, mutants, timers, any/all waits,
// timeouts, handle table, critical sections, spinlocks. Synthetic guest
// structures in a real GuestMemory arena: no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/rtl_exports.h"
#include "kernel/sync.h"

PPC_EXTERN_FUNC(__imp__KeInitializeEvent);
PPC_EXTERN_FUNC(__imp__KeSetEvent);
PPC_EXTERN_FUNC(__imp__KeResetEvent);
PPC_EXTERN_FUNC(__imp__KePulseEvent);
PPC_EXTERN_FUNC(__imp__KeInitializeSemaphore);
PPC_EXTERN_FUNC(__imp__KeReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__KeInitializeMutant);
PPC_EXTERN_FUNC(__imp__KeReleaseMutant);
PPC_EXTERN_FUNC(__imp__KeInitializeTimerEx);
PPC_EXTERN_FUNC(__imp__KeSetTimerEx);
PPC_EXTERN_FUNC(__imp__KeCancelTimer);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__KeWaitForMultipleObjects);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtDuplicateObject);
PPC_EXTERN_FUNC(__imp__NtSetEvent);
PPC_EXTERN_FUNC(__imp__NtClearEvent);
PPC_EXTERN_FUNC(__imp__NtPulseEvent);
PPC_EXTERN_FUNC(__imp__NtCreateSemaphore);
PPC_EXTERN_FUNC(__imp__NtReleaseSemaphore);
PPC_EXTERN_FUNC(__imp__NtCreateMutant);
PPC_EXTERN_FUNC(__imp__NtReleaseMutant);
PPC_EXTERN_FUNC(__imp__NtCreateTimer);
PPC_EXTERN_FUNC(__imp__NtSetTimerEx);
PPC_EXTERN_FUNC(__imp__NtCancelTimer);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtWaitForMultipleObjectsEx);
PPC_EXTERN_FUNC(__imp__NtSignalAndWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__ObReferenceObjectByHandle);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlTryEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
PPC_EXTERN_FUNC(__imp__KeEnterCriticalRegion);
PPC_EXTERN_FUNC(__imp__KeLeaveCriticalRegion);
PPC_EXTERN_FUNC(__imp__KeRaiseIrqlToDpcLevel);
PPC_EXTERN_FUNC(__imp__KfLowerIrql);
PPC_EXTERN_FUNC(__imp__KfAcquireSpinLock);
PPC_EXTERN_FUNC(__imp__KfReleaseSpinLock);
PPC_EXTERN_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql);
PPC_EXTERN_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql);

namespace {

using namespace kernel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Objects = std::vector<std::shared_ptr<KernelObject>>;

constexpr uint32_t kWaitAll = 0;
constexpr uint32_t kWaitAny = 1;

// A single guest arena for the whole test (4 GB reserved only once) with the
// kernel's allocators; Alloc takes zero-committed memory from GuestMemory's
// bump allocator.
struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = mem.Alloc(size, 0x10);
    REQUIRE(a != 0);
    return a;
  }
  uint32_t Get32(uint32_t a) const { return *reinterpret_cast<be<uint32_t>*>(mem.base + a); }
  int32_t GetI32(uint32_t a) const { return int32_t(Get32(a)); }
  void Put32(uint32_t a, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(mem.base + a) = v; }
  void Put64(uint32_t a, uint64_t v) { *reinterpret_cast<be<uint64_t>*>(mem.base + a) = v; }
  uint8_t Get8(uint32_t a) const { return mem.base[a]; }
  void Fill(uint32_t a, uint8_t v, uint32_t n) { std::memset(mem.base + a, v, n); }

  // Calls an export like the generated code: integer arguments in r3..r10.
  uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
    PPCContext ctx{};
    uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                         &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    ctx.lr = 0x82001234;
    fn(ctx, mem.base);
    return ctx.r3.u32;
  }
};

Env& E() {
  static Env env;
  return env;
}

int64_t Rel(std::chrono::milliseconds ms) { return -int64_t(ms.count()) * 10000; }

uint32_t WaitOne(const std::shared_ptr<KernelObject>& o, const int64_t* timeout, bool alertable = false) {
  const std::array<std::shared_ptr<KernelObject>, 1> one{o};
  return WaitAny(one, timeout, alertable);
}

const int64_t kZero = 0;

}  // namespace

TEST_CASE("manual and automatic event", "[sync]") {
  auto manual = std::make_shared<Event>(true, false);
  auto autoe = std::make_shared<Event>(false, false);

  CHECK(WaitOne(manual, &kZero) == kStatusTimeout);
  CHECK(manual->Set() == 0);
  CHECK(manual->Set() == 1);
  CHECK(WaitOne(manual, &kZero) == kStatusWait0);
  CHECK(WaitOne(manual, &kZero) == kStatusWait0);  // stays signaled
  CHECK(manual->Reset() == 1);
  CHECK(WaitOne(manual, &kZero) == kStatusTimeout);

  autoe->Set();
  CHECK(WaitOne(autoe, &kZero) == kStatusWait0);
  CHECK(WaitOne(autoe, &kZero) == kStatusTimeout);  // consumed by the wait

  // KePulseEvent: wakes current waiters and leaves the event not signaled.
  std::atomic<int> woken{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < 3; ++i)
    ts.emplace_back([&] {
      if (WaitOne(manual, nullptr) == kStatusWait0) ++woken;
    });
  while (manual->WaiterCount() < 3) std::this_thread::yield();
  CHECK(manual->Pulse() == 0);
  for (auto& t : ts) t.join();
  CHECK(woken == 3);
  CHECK(WaitOne(manual, &kZero) == kStatusTimeout);

  // Pulse on an automatic event: wakes exactly one.
  std::atomic<int> got{0};
  std::thread a([&] {
    const int64_t t = Rel(300ms);
    if (WaitOne(autoe, &t) == kStatusWait0) ++got;
  });
  std::thread b([&] {
    const int64_t t = Rel(300ms);
    if (WaitOne(autoe, &t) == kStatusWait0) ++got;
  });
  while (autoe->WaiterCount() < 2) std::this_thread::yield();
  autoe->Pulse();
  a.join();
  b.join();
  CHECK(got == 1);
}

TEST_CASE("events via the Ke* and Nt* exports", "[sync]") {
  auto& e = E();
  const uint32_t ev = e.Alloc(0x10);
  const uint32_t tmo = e.Alloc(8);  // the timeout's LARGE_INTEGER
  const uint32_t out = e.Alloc(8);
  e.Put64(tmo, 0);

  e.Call(__imp__KeInitializeEvent, {ev, 1 /*SynchronizationEvent*/, 0});
  CHECK(e.Get8(ev) == 1);
  CHECK(e.GetI32(ev + 4) == 0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__KeSetEvent, {ev, 0, 0}) == 0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__KeSetEvent, {ev, 0, 0}) == 0);
  CHECK(e.Call(__imp__KeResetEvent, {ev}) == 1);
  CHECK(e.Call(__imp__KePulseEvent, {ev, 0, 0}) == 0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);

  // Nt*: 0 = NotificationEvent (manual), initial state signaled.
  CHECK(e.Call(__imp__NtCreateEvent, {out, 0, 0, 1}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  REQUIRE(h != 0);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__NtClearEvent, {h}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__NtSetEvent, {h, out + 4}) == kStatusSuccess);
  CHECK(e.Get32(out + 4) == 0);
  CHECK(e.Call(__imp__NtPulseEvent, {h, out + 4}) == kStatusSuccess);
  CHECK(e.Get32(out + 4) == 1);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusInvalidHandle);
}

TEST_CASE("NtDuplicateObject: second handle to the same object, DUPLICATE_CLOSE_SOURCE", "[sync]") {
  Env& e = E();
  const uint32_t out = e.Alloc(8);
  REQUIRE(e.Call(__imp__NtCreateEvent, {out, 0, 0, 0}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  REQUIRE(e.Call(__imp__NtDuplicateObject, {h, out + 4, 0}) == kStatusSuccess);
  const uint32_t dup = e.Get32(out + 4);
  CHECK(dup != h);
  CHECK(HandleGet(dup) == HandleGet(h));
  // the event signaled through one handle is seen from the other
  CHECK(e.Call(__imp__NtSetEvent, {h, 0}) == kStatusSuccess);
  CHECK(HandleGet(dup)->IsSignaled());
  // options 1: closes the source, the duplicate stays valid
  REQUIRE(e.Call(__imp__NtDuplicateObject, {dup, out + 4, 1}) == kStatusSuccess);
  const uint32_t dup2 = e.Get32(out + 4);
  CHECK(HandleGet(dup) == nullptr);
  CHECK(HandleGet(dup2) == HandleGet(h));
  // new_handle may be null; unknown handle -> STATUS_INVALID_HANDLE and -1
  CHECK(e.Call(__imp__NtDuplicateObject, {h, 0, 0}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtDuplicateObject, {0x12345678, out + 4, 0}) == kStatusInvalidHandle);
  CHECK(e.Get32(out + 4) == 0xFFFFFFFF);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtClose, {dup2}) == kStatusSuccess);
}

TEST_CASE("semaphore with limit", "[sync]") {
  auto sem = std::make_shared<Semaphore>(1, 3);
  int32_t prev = -1;
  CHECK(WaitOne(sem, &kZero) == kStatusWait0);
  CHECK(WaitOne(sem, &kZero) == kStatusTimeout);
  CHECK(sem->Release(2, &prev));
  CHECK(prev == 0);
  CHECK_FALSE(sem->Release(2, &prev));  // 2 + 2 > 3: rejected, count unchanged
  CHECK(prev == 2);
  CHECK(sem->Release(1, &prev));
  CHECK(prev == 2);
  for (int i = 0; i < 3; ++i) CHECK(WaitOne(sem, &kZero) == kStatusWait0);
  CHECK(WaitOne(sem, &kZero) == kStatusTimeout);

  auto& e = E();
  const uint32_t ks = e.Alloc(0x14);
  const uint32_t tmo = e.Alloc(8);
  const uint32_t out = e.Alloc(8);
  e.Call(__imp__KeInitializeSemaphore, {ks, 0, 2});
  CHECK(e.Get8(ks) == 5);
  CHECK(e.Get32(ks + 0x10) == 2);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ks, 0, 0, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__KeReleaseSemaphore, {ks, 0, 2, 0}) == 0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ks, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ks, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ks, 0, 0, 0, tmo}) == kStatusTimeout);

  CHECK(e.Call(__imp__NtCreateSemaphore, {out, 0, 3, 2}) == kStatusInvalidParameter);  // count > limit
  CHECK(e.Call(__imp__NtCreateSemaphore, {out, 0, 1, 2}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  CHECK(e.Call(__imp__NtReleaseSemaphore, {h, 1, out + 4}) == kStatusSuccess);
  CHECK(e.Get32(out + 4) == 1);
  CHECK(e.Call(__imp__NtReleaseSemaphore, {h, 1, out + 4}) == kStatusSemaphoreLimitExceeded);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
}

TEST_CASE("recursive and abandoned mutant", "[sync]") {
  auto m = std::make_shared<Mutant>(false);
  int32_t prev = 0;
  CHECK(WaitOne(m, &kZero) == kStatusWait0);
  CHECK(WaitOne(m, &kZero) == kStatusWait0);  // recursive: the same thread takes it again
  CHECK(m->Owner() == CurrentThreadId());

  // another thread does not get it until it is released twice
  uint32_t other = 0;
  std::thread([&] { other = WaitOne(m, &kZero); }).join();
  CHECK(other == kStatusTimeout);
  CHECK(m->Release(false, &prev) == kStatusSuccess);
  CHECK(prev == -1);
  std::thread([&] { other = WaitOne(m, &kZero); }).join();
  CHECK(other == kStatusTimeout);
  CHECK(m->Release(false, &prev) == kStatusSuccess);
  CHECK(prev == 0);
  CHECK(m->Release(false, &prev) == kStatusMutantNotOwned);

  // abandoned: the owner exits without releasing it
  uint32_t owner_id = 0;
  std::thread([&] {
    CHECK(WaitOne(m, &kZero) == kStatusWait0);
    owner_id = CurrentThreadId();
  }).join();
  CHECK(m->Owner() == owner_id);
  CHECK(WaitOne(m, &kZero) == kStatusTimeout);
  AbandonMutantsOwnedBy(owner_id);
  CHECK(WaitOne(m, &kZero) == kStatusAbandonedWait0);
  CHECK(m->Owner() == CurrentThreadId());
  CHECK(m->Release(false, &prev) == kStatusSuccess);
  CHECK(WaitOne(m, &kZero) == kStatusWait0);  // the abandonment is consumed once
  CHECK(m->Release(false, &prev) == kStatusSuccess);

  // export: KeInitializeMutant with initial owner
  auto& e = E();
  const uint32_t km = e.Alloc(0x20);
  const uint32_t out = e.Alloc(8);
  e.Call(__imp__KeInitializeMutant, {km, 1});
  CHECK(e.Get8(km) == 2);
  CHECK(e.GetI32(km + 4) == 0);
  CHECK(e.Call(__imp__KeReleaseMutant, {km, 0, 0, 0}) == 0);
  std::thread([&] { other = e.Call(__imp__KeWaitForSingleObject, {km, 0, 0, 0, 0}); }).join();
  CHECK(other == kStatusWait0);

  CHECK(e.Call(__imp__NtCreateMutant, {out, 0, 1}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  CHECK(e.Call(__imp__NtReleaseMutant, {h, 0}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtReleaseMutant, {h, 0}) == kStatusMutantNotOwned);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
}

TEST_CASE("periodic and one-shot timer", "[sync]") {
  auto t = std::make_shared<Timer>(true /*synchronization*/);
  const int64_t due = Rel(20ms);
  CHECK_FALSE(t->Set(due, 15, 0, 0));
  const auto start = Clock::now();
  const int64_t limit = Rel(2000ms);
  for (int i = 0; i < 4; ++i) REQUIRE(WaitOne(t, &limit) == kStatusWait0);
  const auto elapsed = Clock::now() - start;
  CHECK(elapsed >= 20ms + 3 * 15ms - 5ms);
  CHECK(t->Cancel());
  CHECK_FALSE(t->Cancel());
  const int64_t short_wait = Rel(60ms);
  CHECK(WaitOne(t, &short_wait) == kStatusTimeout);  // cancelled: no more fires

  // notification timer: stays signaled after the fire
  auto n = std::make_shared<Timer>(false);
  n->Set(Rel(5ms), 0, 0, 0);
  REQUIRE(WaitOne(n, &limit) == kStatusWait0);
  CHECK(WaitOne(n, &kZero) == kStatusWait0);

  // Ke* exports: KeInitializeTimerEx + KeSetTimerEx (DueTime by value in r4)
  auto& e = E();
  const uint32_t kt = e.Alloc(0x28);
  const uint32_t tmo = e.Alloc(8);
  e.Put64(tmo, uint64_t(Rel(2000ms)));
  e.Call(__imp__KeInitializeTimerEx, {kt, 1});
  CHECK(e.Get8(kt) == 9);
  CHECK(e.Call(__imp__KeSetTimerEx, {kt, uint64_t(Rel(10ms)), 0, 0}) == 0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {kt, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeCancelTimer, {kt}) == 0);

  // export Nt*: NtCreateTimer + NtSetTimerEx(handle, &due, routine, mode, arg, resume, period)
  const uint32_t out = e.Alloc(16);
  CHECK(e.Call(__imp__NtCreateTimer, {out, 0, 1}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  e.Put64(out + 8, uint64_t(Rel(10ms)));
  CHECK(e.Call(__imp__NtSetTimerEx, {h, out + 8, 0, 1, 0, 0, 10, 0}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__NtCancelTimer, {h, out + 4}) == kStatusSuccess);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
}

TEST_CASE("WaitAny returns the right index", "[sync]") {
  Objects objs;
  for (int i = 0; i < 4; ++i) objs.push_back(std::make_shared<Event>(false, false));
  auto ev = [&](int i) -> Event& { return static_cast<Event&>(*objs[i]); };
  CHECK(WaitAny(objs, &kZero, false) == kStatusTimeout);
  ev(2).Set();
  CHECK(WaitAny(objs, &kZero, false) == kStatusWait0 + 2);
  // several objects signaled: the lowest index wins
  ev(3).Set();
  ev(1).Set();
  CHECK(WaitAny(objs, &kZero, false) == kStatusWait0 + 1);
  CHECK(WaitAny(objs, &kZero, false) == kStatusWait0 + 3);

  // woken by another thread
  uint32_t r = 0;
  std::thread w([&] { r = WaitAny(objs, nullptr, false); });
  while (objs[0]->WaiterCount() < 1) std::this_thread::yield();
  ev(3).Set();
  w.join();
  CHECK(r == kStatusWait0 + 3);
  for (auto& o : objs) CHECK(o->WaiterCount() == 0);

  // exports: KeWaitForMultipleObjects (array of pointers) and Nt (array of handles)
  auto& e = E();
  const uint32_t evs = e.Alloc(0x30);
  const uint32_t arr = e.Alloc(0x20);
  const uint32_t tmo = e.Alloc(8);
  for (uint32_t i = 0; i < 3; ++i) {
    e.Call(__imp__KeInitializeEvent, {evs + 0x10 * i, 1, i == 2});
    e.Put32(arr + 4 * i, evs + 0x10 * i);
  }
  CHECK(e.Call(__imp__KeWaitForMultipleObjects, {3, arr, kWaitAny, 0, 0, 0, tmo, 0}) == 2);
  CHECK(e.Call(__imp__KeWaitForMultipleObjects, {3, arr, kWaitAny, 0, 0, 0, tmo, 0}) == kStatusTimeout);

  const uint32_t hs = e.Alloc(0x10);
  for (uint32_t i = 0; i < 2; ++i) {
    REQUIRE(e.Call(__imp__NtCreateEvent, {hs + 4 * i, 0, 1, i == 1}) == kStatusSuccess);
  }
  CHECK(e.Call(__imp__NtWaitForMultipleObjectsEx, {2, hs, kWaitAny, 1, 0, tmo}) == 1);
  CHECK(e.Call(__imp__NtWaitForMultipleObjectsEx, {2, hs, kWaitAny, 1, 0, tmo}) == kStatusTimeout);
  CHECK(e.Call(__imp__NtWaitForMultipleObjectsEx, {0, hs, kWaitAny, 1, 0, tmo}) == kStatusInvalidParameter);
  CHECK(e.Call(__imp__NtWaitForMultipleObjectsEx, {65, hs, kWaitAny, 1, 0, tmo}) == kStatusInvalidParameter);
  for (uint32_t i = 0; i < 2; ++i) CHECK(e.Call(__imp__NtClose, {e.Get32(hs + 4 * i)}) == kStatusSuccess);
}

TEST_CASE("atomic WaitAll", "[sync]") {
  auto a = std::make_shared<Event>(false, false);
  auto b = std::make_shared<Event>(false, false);
  Objects both{a, b};
  a->Set();
  CHECK(WaitAll(both, &kZero, false) == kStatusTimeout);
  CHECK(WaitOne(a, &kZero) == kStatusWait0);  // the failed WaitAll did not consume a
  a->Set();
  b->Set();
  CHECK(WaitAll(both, &kZero, false) == kStatusWait0);
  CHECK(WaitOne(a, &kZero) == kStatusTimeout);  // both consumed
  CHECK(WaitOne(b, &kZero) == kStatusTimeout);

  // One thread does WaitAll(a, b) and another WaitAny(a): with a signaled and b not,
  // WaitAll must not hold on to a, which goes to WaitAny. Then b + a: WaitAll
  // consumes both in one go.
  for (int round = 0; round < 200; ++round) {
    std::atomic<int> all_done{0}, any_done{0};
    std::thread t_all([&] {
      const int64_t t = Rel(2000ms);
      if (WaitAll(both, &t, false) == kStatusWait0) ++all_done;
    });
    std::thread t_any([&] {
      const int64_t t = Rel(2000ms);
      if (WaitOne(a, &t) == kStatusWait0) ++any_done;
    });
    while (a->WaiterCount() < 2) std::this_thread::yield();
    a->Set();
    t_any.join();
    CHECK(any_done == 1);
    CHECK(all_done == 0);
    b->Set();
    CHECK(all_done == 0);  // only b: WaitAll still waits, b stays signaled
    a->Set();
    t_all.join();
    CHECK(all_done == 1);
    CHECK(WaitOne(a, &kZero) == kStatusTimeout);
    CHECK(WaitOne(b, &kZero) == kStatusTimeout);
  }

  auto& e = E();
  const uint32_t evs = e.Alloc(0x20);
  const uint32_t arr = e.Alloc(8);
  const uint32_t tmo = e.Alloc(8);
  for (uint32_t i = 0; i < 2; ++i) {
    e.Call(__imp__KeInitializeEvent, {evs + 0x10 * i, 0, 1});
    e.Put32(arr + 4 * i, evs + 0x10 * i);
  }
  CHECK(e.Call(__imp__KeWaitForMultipleObjects, {2, arr, kWaitAll, 0, 0, 0, tmo, 0}) == kStatusWait0);
  e.Call(__imp__KeResetEvent, {evs});
  CHECK(e.Call(__imp__KeWaitForMultipleObjects, {2, arr, kWaitAll, 0, 0, 0, tmo, 0}) == kStatusTimeout);
}

TEST_CASE("timeouts: null, zero, relative, absolute", "[sync]") {
  auto ev = std::make_shared<Event>(true, false);

  // null = infinite: unblocked by another thread
  std::atomic<uint32_t> r{0xFFFF};
  std::thread w([&] { r = WaitOne(ev, nullptr); });
  std::this_thread::sleep_for(50ms);
  CHECK(r == 0xFFFF);  // still waiting
  ev->Set();
  w.join();
  CHECK(r == kStatusWait0);
  ev->Reset();

  // 0 = just try
  auto start = Clock::now();
  CHECK(WaitOne(ev, &kZero) == kStatusTimeout);
  CHECK(Clock::now() - start < 500ms);

  // relative 10 ms
  const int64_t ten = Rel(10ms);
  start = Clock::now();
  CHECK(WaitOne(ev, &ten) == kStatusTimeout);
  auto elapsed = Clock::now() - start;
  CHECK(elapsed >= 9ms);
  CHECK(elapsed < 1000ms);

  // absolute already expired (FILETIME in the past, same clock as KeQuerySystemTime)
  const int64_t past = int64_t(rtl::QuerySystemTime()) - 10'000'000;
  start = Clock::now();
  CHECK(WaitOne(ev, &past) == kStatusTimeout);
  CHECK(Clock::now() - start < 500ms);

  // absolute 30 ms in the future
  const int64_t future = int64_t(rtl::QuerySystemTime()) + 300'000;
  start = Clock::now();
  CHECK(WaitOne(ev, &future) == kStatusTimeout);
  elapsed = Clock::now() - start;
  CHECK(elapsed >= 20ms);
  CHECK(elapsed < 1000ms);

  // huge relative: no overflow turning it into "already expired"
  const int64_t huge = INT64_MIN;
  r = 0xFFFF;
  std::thread w2([&] { r = WaitOne(ev, &huge); });
  std::this_thread::sleep_for(30ms);
  CHECK(r == 0xFFFF);
  ev->Set();
  w2.join();
  CHECK(r == kStatusWait0);
}

TEST_CASE("alertable wait: hook for APCs and alerts", "[sync]") {
  static std::atomic<uint32_t> pending_for{0};
  static std::atomic<uint32_t> pending_status{0};
  SetAlertPoll([](uint32_t thread) -> uint32_t {
    if (pending_for.load() == thread) {
      pending_for = 0;
      return pending_status.load();
    }
    return 0;
  });
  auto ev = std::make_shared<Event>(true, false);
  std::atomic<uint32_t> waiter{0};
  uint32_t r = 0;
  std::thread w([&] {
    waiter = CurrentThreadId();
    r = WaitOne(ev, nullptr, true);
  });
  while (!waiter || ev->WaiterCount() < 1) std::this_thread::yield();
  pending_status = kStatusUserApc;
  pending_for = waiter.load();
  WakeWaiters();
  w.join();
  CHECK(r == kStatusUserApc);
  CHECK(ev->WaiterCount() == 0);

  // not alertable: the alert does not interrupt the wait
  const int64_t t = Rel(50ms);
  std::thread w2([&] {
    pending_status = kStatusAlerted;
    pending_for = CurrentThreadId();
    r = WaitOne(ev, &t, false);
  });
  w2.join();
  CHECK(r == kStatusTimeout);

  // DelayExecution: end of interval = STATUS_SUCCESS; alertable one interrupted
  auto start = Clock::now();
  CHECK(DelayExecution(Rel(10ms), true) == kStatusSuccess);
  CHECK(Clock::now() - start >= 9ms);
  waiter = 0;
  std::thread w3([&] {
    waiter = CurrentThreadId();
    r = DelayExecution(Rel(5000ms), true);
  });
  while (!waiter) std::this_thread::yield();
  std::this_thread::sleep_for(20ms);
  pending_status = kStatusUserApc;
  pending_for = waiter.load();
  WakeWaiters();
  start = Clock::now();
  w3.join();
  CHECK(r == kStatusUserApc);
  CHECK(Clock::now() - start < 2000ms);
  pending_for = 0;
  SetAlertPoll(nullptr);
}

TEST_CASE("WaitAll with the same object twice", "[sync]") {
  auto sem = std::make_shared<Semaphore>(1, 2);
  Objects twice{sem, sem};
  CHECK(WaitAll(twice, &kZero, false) == kStatusInvalidParameter);
  CHECK(WaitAny(twice, &kZero, false) == kStatusWait0);
  CHECK(WaitAny(Objects{}, &kZero, false) == kStatusInvalidParameter);
}

TEST_CASE("KeInitializeEvent repeated on the same structure", "[sync]") {
  auto& e = E();
  const uint32_t ev = e.Alloc(0x10);
  const uint32_t tmo = e.Alloc(8);

  // first use: manual event left signaled
  e.Call(__imp__KeInitializeEvent, {ev, 0, 1});
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  auto first = ObjectFromGuest(ev);

  // same structure reinitialized as automatic not signaled: clean state
  e.Call(__imp__KeInitializeEvent, {ev, 1, 0});
  auto second = ObjectFromGuest(ev);
  CHECK(second != first);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);
  e.Call(__imp__KeSetEvent, {ev, 0, 0});
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);

  // memory reused without KeInitialize (the game's inline initialization):
  // header rewritten from scratch -> new object from the header, not the old one
  e.Fill(ev, 0, 0x10);
  *reinterpret_cast<be<int32_t>*>(e.mem.base + ev + 4) = 1;  // signaled NotificationEvent
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(ObjectFromGuest(ev) != second);

  // the same memory reused for a semaphore
  e.Call(__imp__KeInitializeSemaphore, {ev, 1, 1});
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusWait0);
  CHECK(e.Call(__imp__KeWaitForSingleObject, {ev, 0, 0, 0, tmo}) == kStatusTimeout);
}

TEST_CASE("unknown handle -> STATUS_INVALID_HANDLE", "[sync]") {
  auto& e = E();
  const uint32_t out = e.Alloc(8);
  const uint32_t tmo = e.Alloc(8);
  const uint32_t bogus = 0x7FFFFFF0;
  CHECK(HandleGet(bogus) == nullptr);
  CHECK_FALSE(HandleClose(bogus));
  CHECK(e.Call(__imp__NtClose, {bogus}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__NtSetEvent, {bogus, 0}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__NtClearEvent, {bogus}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__NtReleaseSemaphore, {bogus, 1, 0}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__NtReleaseMutant, {bogus, 0}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__NtWaitForSingleObjectEx, {bogus, 1, 0, tmo}) == kStatusInvalidHandle);
  e.Put32(out, bogus);
  CHECK(e.Call(__imp__NtWaitForMultipleObjectsEx, {1, out, kWaitAny, 1, 0, tmo}) == kStatusInvalidHandle);
  CHECK(e.Call(__imp__ObReferenceObjectByHandle, {bogus, 0, out}) == kStatusInvalidHandle);

  // handle of the wrong type: a semaphore is not an event
  REQUIRE(e.Call(__imp__NtCreateSemaphore, {out, 0, 0, 1}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  CHECK(e.Call(__imp__NtSetEvent, {h, 0}) == kStatusObjectTypeMismatch);
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
}

TEST_CASE("handle table and references", "[sync]") {
  auto ev = std::make_shared<Event>(true, false);
  const uint32_t h1 = HandleCreate(ev);
  const uint32_t h2 = HandleCreate(ev);
  CHECK(h1 != 0);
  CHECK(h2 != 0);
  CHECK(h1 != h2);
  CHECK(HandleGet(h1) == ev);
  std::weak_ptr<KernelObject> weak = ev;
  ev.reset();
  CHECK(HandleClose(h1));
  CHECK_FALSE(weak.expired());  // h2 keeps it alive
  CHECK(HandleClose(h2));
  CHECK(weak.expired());

  // ObReferenceObjectByHandle keeps the object alive beyond NtClose until
  // ObDereferenceObject; the guest pointer is a waitable object.
  auto& e = E();
  const uint32_t out = e.Alloc(8);
  const uint32_t tmo = e.Alloc(8);
  REQUIRE(e.Call(__imp__NtCreateEvent, {out, 0, 0, 0}) == kStatusSuccess);
  const uint32_t h = e.Get32(out);
  std::weak_ptr<KernelObject> w = HandleGet(h);
  CHECK(e.Call(__imp__ObReferenceObjectByHandle, {h, 0, out + 4}) == kStatusSuccess);
  const uint32_t body = e.Get32(out + 4);
  CHECK(body != 0);
  CHECK(ObjectFromGuest(body) == w.lock());
  CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
  CHECK_FALSE(w.expired());
  CHECK(e.Call(__imp__NtSetEvent, {h, 0}) == kStatusInvalidHandle);
  e.Call(__imp__KeSetEvent, {body, 0, 0});
  CHECK(e.Call(__imp__KeWaitForSingleObject, {body, 0, 0, 0, tmo}) == kStatusWait0);
  e.Call(__imp__ObDereferenceObject, {body});
  CHECK(w.expired());
}

TEST_CASE("NtSignalAndWaitForSingleObjectEx", "[sync]") {
  auto& e = E();
  const uint32_t out = e.Alloc(16);
  const uint32_t tmo = e.Alloc(8);
  e.Put64(tmo, uint64_t(Rel(2000ms)));
  REQUIRE(e.Call(__imp__NtCreateEvent, {out, 0, 1, 0}) == kStatusSuccess);
  REQUIRE(e.Call(__imp__NtCreateEvent, {out + 4, 0, 1, 0}) == kStatusSuccess);
  const uint32_t ping = e.Get32(out), pong = e.Get32(out + 4);
  uint32_t peer_wait = 0;
  std::thread peer([&] {
    // waits for ping, then signals pong
    peer_wait = e.Call(__imp__NtWaitForSingleObjectEx, {ping, 1, 0, tmo});
    e.Call(__imp__NtSetEvent, {pong, 0});
  });
  const uint32_t r = e.Call(__imp__NtSignalAndWaitForSingleObjectEx, {ping, pong, 0, 0, tmo});
  peer.join();
  CHECK(peer_wait == kStatusWait0);
  CHECK(r == kStatusWait0);
  CHECK(e.Call(__imp__NtSignalAndWaitForSingleObjectEx, {0x7FFFFFF0, pong, 0, 0, tmo}) == kStatusInvalidHandle);
  e.Call(__imp__NtClose, {ping});
  e.Call(__imp__NtClose, {pong});
}

TEST_CASE("critical section: recursion and contention among 4 threads", "[sync]") {
  auto& e = E();
  const uint32_t cs = e.Alloc(0x1C);
  e.Fill(cs, 0xCD, 0x1C);  // dirty: the initialization must clean it
  e.Call(__imp__RtlInitializeCriticalSection, {cs});
  CHECK(e.Get8(cs) == 1);  // header: synchronization event
  CHECK(e.GetI32(cs + 0x10) == -1);
  CHECK(e.GetI32(cs + 0x14) == 0);
  CHECK(e.Get32(cs + 0x18) == 0);

  e.Call(__imp__RtlEnterCriticalSection, {cs});
  CHECK(e.GetI32(cs + 0x10) == 0);
  CHECK(e.GetI32(cs + 0x14) == 1);
  CHECK(e.Get32(cs + 0x18) == CurrentThreadId());
  e.Call(__imp__RtlEnterCriticalSection, {cs});
  CHECK(e.Call(__imp__RtlTryEnterCriticalSection, {cs}) == 1);
  CHECK(e.GetI32(cs + 0x10) == 2);
  CHECK(e.GetI32(cs + 0x14) == 3);
  uint32_t other = 9;
  std::thread([&] { other = e.Call(__imp__RtlTryEnterCriticalSection, {cs}); }).join();
  CHECK(other == 0);
  for (int i = 0; i < 3; ++i) e.Call(__imp__RtlLeaveCriticalSection, {cs});
  CHECK(e.GetI32(cs + 0x10) == -1);
  CHECK(e.GetI32(cs + 0x14) == 0);
  CHECK(e.Get32(cs + 0x18) == 0);

  // contention: 4 threads, section with recursion, protected non-atomic
  // counter; without spin (all contention goes through the header's event) and
  // with spin count 1000. A sleep(0) inside the section forces contention.
  for (const uint32_t spin : {0u, 1000u}) {
    CHECK(e.Call(__imp__RtlInitializeCriticalSectionAndSpinCount, {cs, spin}) == kStatusSuccess);
    CHECK(e.Get8(cs + 1) == (spin + 255) / 256);  // spin count / 256 rounded up
    uint64_t counter = 0;
    std::atomic<int> inside{0}, violations{0}, field_errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([&] {
        const uint32_t me = CurrentThreadId();
        for (int i = 0; i < 5000; ++i) {
          e.Call(__imp__RtlEnterCriticalSection, {cs});
          e.Call(__imp__RtlEnterCriticalSection, {cs});
          if (++inside != 1) ++violations;
          if (e.Get32(cs + 0x18) != me || e.GetI32(cs + 0x14) != 2 || e.GetI32(cs + 0x10) < 1) ++field_errors;
          ++counter;
          if ((i & 63) == 0) std::this_thread::sleep_for(0ms);
          --inside;
          e.Call(__imp__RtlLeaveCriticalSection, {cs});
          e.Call(__imp__RtlLeaveCriticalSection, {cs});
        }
      });
    for (auto& t : ts) t.join();
    CHECK(violations == 0);
    CHECK(field_errors == 0);
    CHECK(counter == 4 * 5000);
    CHECK(e.GetI32(cs + 0x10) == -1);
    CHECK(e.GetI32(cs + 0x14) == 0);
    CHECK(e.Get32(cs + 0x18) == 0);
    // without spin the contention went through the event: the header has a host
    // object ('GW2K' signature in WaitListHead.Flink)
    if (spin == 0) CHECK(e.Get32(cs + 8) == 0x4757324B);
  }

  // critical section already initialized in the image data (never
  // RtlInitializeCriticalSection): header and fields written by hand
  const uint32_t cs2 = e.Alloc(0x1C);
  e.Fill(cs2, 0, 0x1C);
  e.mem.base[cs2] = 1;
  e.Put32(cs2 + 0x10, 0xFFFFFFFF);
  e.Call(__imp__RtlEnterCriticalSection, {cs2});
  std::thread([&] { other = e.Call(__imp__RtlTryEnterCriticalSection, {cs2}); }).join();
  CHECK(other == 0);
  e.Call(__imp__RtlLeaveCriticalSection, {cs2});
  CHECK(e.GetI32(cs2 + 0x10) == -1);
}

TEST_CASE("spinlock, IRQL and critical region", "[sync]") {
  auto& e = E();
  const uint32_t lock = e.Alloc(4);
  CHECK(e.Call(__imp__KeRaiseIrqlToDpcLevel, {}) == 0);
  CHECK(e.Call(__imp__KeRaiseIrqlToDpcLevel, {}) == 2);
  e.Call(__imp__KfLowerIrql, {0});

  const uint32_t old = e.Call(__imp__KfAcquireSpinLock, {lock});
  CHECK(old == 0);
  CHECK(e.Get32(lock) != 0);
  CHECK(e.Call(__imp__KeRaiseIrqlToDpcLevel, {}) == 2);  // already at DISPATCH
  e.Call(__imp__KfReleaseSpinLock, {lock, old});
  CHECK(e.Get32(lock) == 0);
  CHECK(e.Call(__imp__KeRaiseIrqlToDpcLevel, {}) == 0);  // IRQL restored
  e.Call(__imp__KfLowerIrql, {0});

  uint64_t counter = 0;
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&] {
      for (int i = 0; i < 20000; ++i) {
        if (i & 1) {
          const uint32_t irql = e.Call(__imp__KfAcquireSpinLock, {lock});
          ++counter;
          e.Call(__imp__KfReleaseSpinLock, {lock, irql});
        } else {
          e.Call(__imp__KeAcquireSpinLockAtRaisedIrql, {lock});
          ++counter;
          e.Call(__imp__KeReleaseSpinLockFromRaisedIrql, {lock});
        }
      }
    });
  for (auto& t : ts) t.join();
  CHECK(counter == 4 * 20000);

  e.Call(__imp__KeEnterCriticalRegion, {});
  e.Call(__imp__KeEnterCriticalRegion, {});
  CHECK(CriticalRegionDepth() == 2);
  e.Call(__imp__KeLeaveCriticalRegion, {});
  e.Call(__imp__KeLeaveCriticalRegion, {});
  CHECK(CriticalRegionDepth() == 0);
}

TEST_CASE("stress: 8 threads create, wait and close handles for 2 s", "[sync]") {
  auto& e = E();
  // shared handles that the threads steal and close from each other
  constexpr int kSlots = 16;
  std::array<std::atomic<uint32_t>, kSlots> shared{};
  std::array<uint32_t, 8> scratch{};
  for (auto& s : scratch) s = e.Alloc(0x40);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> ops{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t] {
      std::mt19937 rng(t * 7919u + 1);
      const uint32_t s = scratch[t];
      e.Put64(s + 8, uint64_t(Rel(1ms)));
      while (!stop.load()) {
        const int slot = int(rng() % kSlots);
        switch (rng() % 5) {
          case 0: {  // creates and publishes
            if (e.Call(__imp__NtCreateEvent, {s, 0, rng() & 1, rng() & 1}) != kStatusSuccess) ++errors;
            const uint32_t prev = shared[slot].exchange(e.Get32(s));
            if (prev && e.Call(__imp__NtClose, {prev}) != kStatusSuccess) ++errors;
            break;
          }
          case 1: {  // steals and closes
            const uint32_t h = shared[slot].exchange(0);
            if (h && e.Call(__imp__NtClose, {h}) != kStatusSuccess) ++errors;
            break;
          }
          case 2: {  // waits with a short timeout (the handle may vanish)
            const uint32_t h = shared[slot].load();
            if (!h) break;
            const uint32_t r = e.Call(__imp__NtWaitForSingleObjectEx, {h, 1, 0, s + 8});
            if (r != kStatusWait0 && r != kStatusTimeout && r != kStatusInvalidHandle) ++errors;
            break;
          }
          case 3: {  // signals
            const uint32_t h = shared[slot].load();
            if (!h) break;
            const uint32_t r = e.Call(__imp__NtSetEvent, {h, 0});
            if (r != kStatusSuccess && r != kStatusInvalidHandle) ++errors;
            break;
          }
          default: {  // multiple wait on two slots
            e.Put32(s + 16, shared[slot].load());
            e.Put32(s + 20, shared[(slot + 1) % kSlots].load());
            if (!e.Get32(s + 16) || !e.Get32(s + 20)) break;
            const uint32_t r = e.Call(__imp__NtWaitForMultipleObjectsEx, {2, s + 16, rng() & 1, 1, 0, s + 8});
            if (r > 1 && r != kStatusTimeout && r != kStatusInvalidHandle) ++errors;
            break;
          }
        }
        ++ops;
      }
    });
  std::this_thread::sleep_for(2s);
  stop = true;
  for (auto& t : ts) t.join();
  for (auto& sl : shared)
    if (const uint32_t h = sl.exchange(0)) CHECK(e.Call(__imp__NtClose, {h}) == kStatusSuccess);
  CHECK(errors == 0);
  CHECK(ops > 1000);
}

