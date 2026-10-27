// Guest threads (runtime/kernel/threads.*, thread_exports.cpp): suspended
// creation and resume, TLS, r13/PCR/KTHREAD, waiting on a thread, stack
// guard, KeDelayExecutionThread, APCs, suspension, affinity. The "guest
// functions" are host functions registered in a fake function table; real
// GuestMemory arena, no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "crash_map.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/rtl_exports.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"
#include "../temp_dir.h"

PPC_EXTERN_FUNC(__imp__ExCreateThread);
PPC_EXTERN_FUNC(__imp__KiApcNormalRoutineNop);
PPC_EXTERN_FUNC(__imp__InterlockedPopEntrySList);
PPC_EXTERN_FUNC(__imp__InterlockedFlushSList);
PPC_EXTERN_FUNC(__imp__ExTerminateThread);
PPC_EXTERN_FUNC(__imp__NtResumeThread);
PPC_EXTERN_FUNC(__imp__NtSuspendThread);
PPC_EXTERN_FUNC(__imp__KeResumeThread);
PPC_EXTERN_FUNC(__imp__KeSuspendThread);
PPC_EXTERN_FUNC(__imp__KeGetCurrentThread);
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessorNumber);
PPC_EXTERN_FUNC(__imp__KeSetAffinityThread);
PPC_EXTERN_FUNC(__imp__KeSetBasePriorityThread);
PPC_EXTERN_FUNC(__imp__KeQueryBasePriorityThread);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);
PPC_EXTERN_FUNC(__imp__NtYieldExecution);
PPC_EXTERN_FUNC(__imp__KeTlsAlloc);
PPC_EXTERN_FUNC(__imp__KeTlsFree);
PPC_EXTERN_FUNC(__imp__KeTlsGetValue);
PPC_EXTERN_FUNC(__imp__KeTlsSetValue);
PPC_EXTERN_FUNC(__imp__XGetAVPack);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__KeWaitForSingleObject);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateTimer);
PPC_EXTERN_FUNC(__imp__NtSetTimerEx);

namespace {

using namespace kernel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int64_t Rel(std::chrono::milliseconds ms) { return -int64_t(ms.count()) * 10000; }

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }
void Put64(uint32_t a, uint64_t v) { *GuestPtr<be<uint64_t>>(a) = v; }
uint8_t Get8(uint32_t a) { return *GuestPtr<uint8_t>(a); }

// Calls an export like the generated code: integer arguments in r3..r10.
uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

// Waits for `cond` up to `limit`; returns its final value.
template <class F>
bool Eventually(F cond, std::chrono::milliseconds limit = 3000ms) {
  const auto end = Clock::now() + limit;
  while (!cond()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

uint32_t WaitThread(const std::shared_ptr<GuestThread>& t, std::chrono::milliseconds limit = 5000ms) {
  const std::shared_ptr<KernelObject> one[1] = {t};
  const int64_t timeout = Rel(limit);
  return WaitAny(one, &timeout, false);
}

// ---- fake guest functions -------------------------------------------------------

constexpr uint32_t kFnPlusOne = 0x82000000;
constexpr uint32_t kFnTls = 0x82000010;
constexpr uint32_t kFnIdentity = 0x82000020;
constexpr uint32_t kFnBlock = 0x82000030;
constexpr uint32_t kFnXapi = 0x82000040;
constexpr uint32_t kFnDelay = 0x82000050;
constexpr uint32_t kFnApc = 0x82000060;
constexpr uint32_t kFnTerminate = 0x82000070;
constexpr uint32_t kFnMutant = 0x82000080;
constexpr uint32_t kFnSelfSuspend = 0x82000090;
constexpr uint32_t kFnYieldLoop = 0x820000A0;
constexpr uint32_t kFnCpu = 0x820000B0;
constexpr uint32_t kFnNothing = 0x820000C0;
constexpr uint32_t kFnTimer = 0x820000D0;
constexpr uint32_t kFnSelfHandle = 0x820000E0;
constexpr uint32_t kFnMissing = 0x820000F0;  // not registered

std::atomic<bool> g_ran{false};
std::atomic<bool> g_release{false};
std::atomic<bool> g_after{false};

// r3 + 1, the exit code.
PPC_FUNC(FnPlusOne) {
  g_ran = true;
  ctx.r3.u64 = ctx.r3.u32 + 1;
}

// TLS: value r3 in slot g_tls_slot, then re-read after both
// threads have written.
std::atomic<uint32_t> g_tls_slot{0};
std::atomic<int> g_tls_written{0};
std::atomic<uint32_t> g_tls_read[2];
std::atomic<uint32_t> g_tls_raw[2];
PPC_FUNC(FnTls) {
  const uint32_t value = ctx.r3.u32;
  const int idx = value == 0x1111 ? 0 : 1;
  const uint32_t slot = g_tls_slot;
  Call(__imp__KeTlsSetValue, {slot, value});
  ++g_tls_written;
  Eventually([] { return g_tls_written.load() == 2; });
  g_tls_read[idx] = Call(__imp__KeTlsGetValue, {slot});
  // the TLS table is the one pointed to by PCR+0 (r13)
  const uint32_t tls = Get32(ctx.r13.u32 + kPcrTlsPtr);
  g_tls_raw[idx] = Get32(tls + slot * 4);
  ctx.r3.u64 = 0;
}

// Thread identity as seen by the guest code.
struct Seen {
  std::atomic<uint32_t> r13{0}, r1{0}, current_thread{0}, pcr_thread{0}, thread_id_id{0}, kthread_id{0};
} g_seen[2];
PPC_FUNC(FnIdentity) {
  Seen& s = g_seen[ctx.r3.u32 & 1];
  s.r13 = ctx.r13.u32;
  s.r1 = ctx.r1.u32;
  s.current_thread = Call(__imp__KeGetCurrentThread, {});
  s.pcr_thread = Get32(ctx.r13.u32 + kPcrCurrentThread);
  s.thread_id_id = CurrentThreadId();
  s.kthread_id = Get32(s.pcr_thread + kKThreadId);
  ctx.r3.u64 = 0;
}

// Waits for g_release, then exits with 7.
PPC_FUNC(FnBlock) {
  Eventually([] { return g_release.load(); }, 10000ms);
  ctx.r3.u64 = 7;
}

// XAPI trampoline: records (r3, r4) = (entry, context).
std::atomic<uint32_t> g_xapi_r3{0}, g_xapi_r4{0};
PPC_FUNC(FnXapi) {
  g_xapi_r3 = ctx.r3.u32;
  g_xapi_r4 = ctx.r4.u32;
  ctx.r3.u64 = 0x99;  // ignored: with xapi the exit code is 0
}

// APC: records arguments and thread.
std::atomic<uint32_t> g_apc_args[3];
std::atomic<GuestThread*> g_apc_thread{nullptr};
std::atomic<int> g_apc_count{0};
PPC_FUNC(FnApc) {
  g_apc_args[0] = ctx.r3.u32;
  g_apc_args[1] = ctx.r4.u32;
  g_apc_args[2] = ctx.r5.u32;
  g_apc_thread = GuestThread::Current();
  ++g_apc_count;
}

// KeDelayExecutionThread(mode 1, g_delay_alertable, interval); status and
// duration in g_delay_*. r3 = guest address of the timeout.
std::atomic<bool> g_delay_alertable{false};
std::atomic<uint32_t> g_delay_status{0xFFFFFFFF};
std::atomic<int64_t> g_delay_ms{0};
std::atomic<bool> g_delay_started{false};
std::atomic<uint32_t> g_wait_event{0};  // if != 0: alertable KeWaitForSingleObject on this KEVENT
PPC_FUNC(FnDelay) {
  const uint32_t tmo = ctx.r3.u32;
  const uint32_t alertable = g_delay_alertable ? 1 : 0;
  g_delay_started = true;
  const auto start = Clock::now();
  if (const uint32_t ev = g_wait_event.load())
    g_delay_status = Call(__imp__KeWaitForSingleObject, {ev, 0, 1, alertable, tmo});
  else
    g_delay_status = Call(__imp__KeDelayExecutionThread, {1, alertable, tmo});
  g_delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  ctx.r3.u64 = 0;
}

// ExTerminateThread(0x55): the rest must not run.
PPC_FUNC(FnTerminate) {
  Call(__imp__ExTerminateThread, {0x55});
  g_after = true;
}

// Acquires g_mutant and exits without releasing it.
std::shared_ptr<Mutant> g_mutant;
PPC_FUNC(FnMutant) {
  const std::shared_ptr<KernelObject> one[1] = {g_mutant};
  WaitAny(one, nullptr, false);
  ctx.r3.u64 = 0;
}

// KeSuspendThread on itself, then g_after.
PPC_FUNC(FnSelfSuspend) {
  Call(__imp__KeSuspendThread, {Call(__imp__KeGetCurrentThread, {})});
  g_after = true;
  ctx.r3.u64 = 0;
}

// NtYieldExecution in a loop, counting, until g_release.
std::atomic<uint64_t> g_loops{0};
PPC_FUNC(FnYieldLoop) {
  while (!g_release) {
    Call(__imp__NtYieldExecution, {});
    ++g_loops;
  }
  ctx.r3.u64 = 0;
}

// Waits for g_release, then records KeGetCurrentProcessorNumber.
std::atomic<uint32_t> g_cpu{0xFF};
PPC_FUNC(FnCpu) {
  Eventually([] { return g_release.load(); }, 10000ms);
  g_cpu = Call(__imp__KeGetCurrentProcessorNumber, {});
  ctx.r3.u64 = 0;
}

PPC_FUNC(FnNothing) { ctx.r3.u64 = 0; }

// Sets g_timer with APC routine FnApc (arg 0x77), then an alertable wait.
std::atomic<uint32_t> g_timer{0}, g_timer_due{0}, g_timer_tmo{0};
PPC_FUNC(FnTimer) {
  Call(__imp__NtSetTimerEx, {g_timer, g_timer_due, kFnApc, 0, 0x77, 0, 0, 0});
  g_delay_status = Call(__imp__KeDelayExecutionThread, {1, 1, g_timer_tmo});
  ctx.r3.u64 = 0;
}

// Pseudo-handle 0xFFFFFFFE (NtCurrentThread): HandleGet gives the thread itself.
std::atomic<bool> g_self_handle_ok{false};
PPC_FUNC(FnSelfHandle) {
  auto obj = HandleGet(kCurrentThreadHandle);
  g_self_handle_ok = obj && obj.get() == GuestThread::Current();
  ctx.r3.u64 = 0;
}

PPCFunc* Resolve(uint32_t guest) {
  switch (guest) {
    case kFnSelfHandle: return &FnSelfHandle;
    case kFnPlusOne: return &FnPlusOne;
    case kFnTls: return &FnTls;
    case kFnIdentity: return &FnIdentity;
    case kFnBlock: return &FnBlock;
    case kFnXapi: return &FnXapi;
    case kFnDelay: return &FnDelay;
    case kFnApc: return &FnApc;
    case kFnTerminate: return &FnTerminate;
    case kFnMutant: return &FnMutant;
    case kFnSelfSuspend: return &FnSelfSuspend;
    case kFnYieldLoop: return &FnYieldLoop;
    case kFnCpu: return &FnCpu;
    case kFnNothing: return &FnNothing;
    case kFnTimer: return &FnTimer;
    default: return nullptr;
  }
}

// A single guest arena for the whole test with the allocators and the threads.
struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(InitThreads(mem, &Resolve, 0x40000));
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = mem.Alloc(size, 0x10);
    REQUIRE(a != 0);
    return a;
  }
};

Env& E() {
  static Env env;
  return env;
}

uint64_t ThreadsCreated() {
  const std::string s = stats::Summary();
  const auto pos = s.find("threads_created=");
  return pos == std::string::npos ? 0 : std::stoull(s.substr(pos + 16));
}

}  // namespace

TEST_CASE("thread created suspended starts only after Resume; exit code = r3", "[threads]") {
  E();
  g_ran = false;
  const uint64_t created = ThreadsCreated();
  auto t = GuestThread::Create(kFnPlusOne, 41, 0, true);
  REQUIRE(t);
  CHECK(ThreadsCreated() == created + 1);
  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(g_ran);
  CHECK_FALSE(t->Exited());
  CHECK(Get8(t->KThreadAddr() + kKThreadSuspendCount) == 1);
  CHECK(t->Resume() == 1);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(g_ran);
  CHECK(t->Exited());
  CHECK(t->ExitCode() == 42);
  CHECK(Get32(t->KThreadAddr() + kKThreadExitStatus) == 42);
  CHECK(t->Resume() == 0);  // already at zero
}

TEST_CASE("independent TLS between two threads (KeTlsAlloc/Set/Get/Free)", "[threads]") {
  E();
  const uint32_t slot = Call(__imp__KeTlsAlloc, {});
  REQUIRE(slot < kTlsSlots);
  CHECK(Call(__imp__KeTlsAlloc, {}) != slot);  // distinct slots
  g_tls_slot = slot;
  g_tls_written = 0;
  auto a = GuestThread::Create(kFnTls, 0x1111, 0, false);
  auto b = GuestThread::Create(kFnTls, 0x2222, 0, false);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(WaitThread(a) == kStatusWait0);
  CHECK(WaitThread(b) == kStatusWait0);
  CHECK(g_tls_read[0] == 0x1111);
  CHECK(g_tls_read[1] == 0x2222);
  CHECK(g_tls_raw[0] == 0x1111);
  CHECK(g_tls_raw[1] == 0x2222);
  CHECK(Call(__imp__KeTlsFree, {slot}) == 1);
  CHECK(Call(__imp__KeTlsFree, {kTlsOutOfIndexes}) == 0);
  // outside a guest thread there is no TLS
  CHECK(Call(__imp__KeTlsGetValue, {slot}) == 0);
  CHECK(Call(__imp__KeTlsSetValue, {slot, 5}) == 0);
}

TEST_CASE("r13, PCR and KeGetCurrentThread consistent per thread", "[threads]") {
  E();
  auto a = GuestThread::Create(kFnIdentity, 0, 0, false);
  auto b = GuestThread::Create(kFnIdentity, 1, 0, false);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(WaitThread(a) == kStatusWait0);
  CHECK(WaitThread(b) == kStatusWait0);
  const std::shared_ptr<GuestThread> ts[2] = {a, b};
  for (int i = 0; i < 2; ++i) {
    INFO("thread " << i);
    CHECK(g_seen[i].r13 == ts[i]->PcrAddr());
    CHECK(g_seen[i].r1 == ts[i]->StackBase() - 0x80);
    CHECK(g_seen[i].current_thread == ts[i]->KThreadAddr());
    CHECK(g_seen[i].pcr_thread == ts[i]->KThreadAddr());
    CHECK(g_seen[i].thread_id_id == ts[i]->KThreadAddr());  // owner of critical sections and mutants
    CHECK(g_seen[i].kthread_id == ts[i]->ThreadId());
    CHECK(Get32(ts[i]->PcrAddr() + kPcrStackBase) == ts[i]->StackBase());
    CHECK(Get32(ts[i]->PcrAddr() + kPcrStackEnd) == ts[i]->StackLimit());
    CHECK(Get8(ts[i]->KThreadAddr()) == kThreadObject);
  }
  CHECK(a->PcrAddr() != b->PcrAddr());
  CHECK(a->ThreadId() != b->ThreadId());
  CHECK(Call(__imp__KeGetCurrentThread, {}) == 0);  // host thread
}

TEST_CASE("ExCreateThread: wait on the thread (handle and KTHREAD) until its end", "[threads]") {
  auto& e = E();
  g_release = false;
  const uint32_t out = e.Alloc(8);
  const uint32_t tmo = e.Alloc(8);
  Put64(tmo, 0);
  CHECK(Call(__imp__ExCreateThread, {out, 0, out + 4, 0, kFnBlock, 0, 0}) == kStatusSuccess);
  const uint32_t handle = Get32(out);
  auto t = std::dynamic_pointer_cast<GuestThread>(HandleGet(handle));
  REQUIRE(t);
  CHECK(Get32(out + 4) == t->ThreadId());
  CHECK(t->StackBase() - t->StackLimit() == 0x40000);  // stack 0 = the XEX header's
  const uint32_t kt = t->KThreadAddr();
  CHECK(Call(__imp__NtWaitForSingleObjectEx, {handle, 0, 0, tmo}) == kStatusTimeout);
  CHECK(Call(__imp__KeWaitForSingleObject, {kt, 0, 0, 0, tmo}) == kStatusTimeout);
  g_release = true;
  Put64(tmo, uint64_t(Rel(5000ms)));
  CHECK(Call(__imp__NtWaitForSingleObjectEx, {handle, 0, 0, tmo}) == kStatusWait0);
  CHECK(Call(__imp__KeWaitForSingleObject, {kt, 0, 0, 0, tmo}) == kStatusWait0);  // stays signaled
  CHECK(t->ExitCode() == 7);
  CHECK(Call(__imp__NtClose, {handle}) == kStatusSuccess);
}

TEST_CASE("ExCreateThread: XAPI trampoline and flag 0x80 (handle = KTHREAD)", "[threads]") {
  auto& e = E();
  const uint32_t out = e.Alloc(8);
  CHECK(Call(__imp__ExCreateThread, {out, 0x8000, 0, kFnXapi, kFnPlusOne, 0x1234, 0x80}) == kStatusSuccess);
  const uint32_t kt = Get32(out);
  auto t = ThreadFromKThread(kt);
  REQUIRE(t);
  CHECK(t->StackBase() - t->StackLimit() == 0x10000);  // minimum 64 KB
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(g_xapi_r3 == kFnPlusOne);
  CHECK(g_xapi_r4 == 0x1234);
  CHECK(t->ExitCode() == 0);
  CHECK(HandleGet(kt) == nullptr);  // not a handle
}

TEST_CASE("stack: rounded to 64 KB, guard reserved and registered for the crash", "[threads]") {
  E();
  g_release = false;
  auto t = GuestThread::Create(kFnBlock, 0, 0x10001, false);
  REQUIRE(t);
  CHECK(t->StackBase() - t->StackLimit() == 0x20000);
  CHECK(t->StackLimit() - t->GuardBase() == kStackGuardSize);
  RegionInfo guard{}, stack{};
  REQUIRE(Memory()->virt.Query(t->GuardBase(), guard));
  REQUIRE(Memory()->virt.Query(t->StackLimit(), stack));
  CHECK(guard.state == kMemReserve);  // not committed: every access fails
  CHECK(stack.state == kMemCommit);
  const auto host = [](uint32_t a) { return reinterpret_cast<uintptr_t>(GuestPtr<uint8_t>(a)); };
  CHECK(InStackGuard(host(t->GuardBase())));
  CHECK(InStackGuard(host(t->StackLimit() - 1)));
  CHECK_FALSE(InStackGuard(host(t->StackLimit())));
  g_release = true;
  CHECK(WaitThread(t) == kStatusWait0);
}

TEST_CASE("write into the stack guard: crash classified as stack overflow", "[threads]") {
  const auto log = gow2_test::UniqueTempPath("gow2_threads_crash_child");
  const std::string cmd = "\"\"" + std::string(GOW2_THREADS_CRASH_CHILD) + "\" \"" + log.string() + "\" >nul 2>&1\"";
  const int rc = std::system(cmd.c_str());
  std::ifstream in(log);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(log);
  INFO(ss.str());
  CHECK(rc == 3);
  CHECK(ss.str().find("code=0xC0000005") != std::string::npos);
  CHECK(ss.str().find("stack overflow") != std::string::npos);
}

TEST_CASE("KeDelayExecutionThread: relative, zero, alertable with APC and alert", "[threads]") {
  auto& e = E();
  const uint32_t tmo = e.Alloc(8);
  g_wait_event = 0;
  g_delay_alertable = false;

  SECTION("relative, not alertable: waits for the interval") {
    Put64(tmo, uint64_t(Rel(60ms)));
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusSuccess);
    CHECK(g_delay_ms >= 55);
  }
  SECTION("interval 0 and NtYieldExecution return immediately") {
    Put64(tmo, 0);
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusSuccess);
    CHECK(g_delay_ms < 50);
    CHECK(Call(__imp__NtYieldExecution, {}) == kStatusSuccess);
  }
  SECTION("alertable: a queued APC interrupts it, runs on the thread, STATUS_USER_APC") {
    g_delay_alertable = true;
    Put64(tmo, uint64_t(Rel(5000ms)));
    g_delay_started = false;
    g_apc_thread = nullptr;
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([] { return g_delay_started.load(); }));
    std::this_thread::sleep_for(30ms);
    t->QueueApc(kFnApc, 0xAA, 0xBB, 0xCC);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusUserApc);
    CHECK(g_delay_ms < 2000);
    CHECK(g_apc_args[0] == 0xAA);
    CHECK(g_apc_args[1] == 0xBB);
    CHECK(g_apc_args[2] == 0xCC);
    CHECK(g_apc_thread == t.get());
  }
  SECTION("alertable: Alert -> STATUS_ALERTED") {
    g_delay_alertable = true;
    Put64(tmo, uint64_t(Rel(5000ms)));
    g_delay_started = false;
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([] { return g_delay_started.load(); }));
    std::this_thread::sleep_for(30ms);
    t->Alert();
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusAlerted);
    CHECK(g_delay_ms < 2000);
  }
  SECTION("not alertable: the APC stays queued and the wait runs to the end") {
    Put64(tmo, uint64_t(Rel(100ms)));
    g_delay_started = false;
    const int before = g_apc_count;
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([] { return g_delay_started.load(); }));
    t->QueueApc(kFnApc, 1, 2, 3);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusSuccess);
    CHECK(g_delay_ms >= 95);
    CHECK(g_apc_count == before);  // dropped at the end of the thread
  }
  SECTION("alertable KeWaitForSingleObject: APC delivered, STATUS_USER_APC") {
    g_delay_alertable = true;
    const uint32_t ev = e.Alloc(0x10);
    std::memset(GuestPtr<uint8_t>(ev), 0, 0x10);  // notification KEVENT, not signaled
    g_wait_event = ev;
    Put64(tmo, uint64_t(Rel(5000ms)));
    g_delay_started = false;
    g_apc_args[0] = 0;
    auto t = GuestThread::Create(kFnDelay, tmo, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([] { return g_delay_started.load(); }));
    std::this_thread::sleep_for(30ms);
    t->QueueApc(kFnApc, 0x42, 0, 0);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_delay_status == kStatusUserApc);
    CHECK(g_apc_args[0] == 0x42);
    g_wait_event = 0;
  }
}

TEST_CASE("NtSetTimerEx APC routine delivered to the thread that set the timer", "[threads]") {
  auto& e = E();
  const uint32_t out = e.Alloc(8);
  REQUIRE(Call(__imp__NtCreateTimer, {out, 0, 0}) == kStatusSuccess);
  g_timer = Get32(out);
  g_timer_due = e.Alloc(8);
  g_timer_tmo = e.Alloc(8);
  Put64(g_timer_due, uint64_t(Rel(20ms)));
  Put64(g_timer_tmo, uint64_t(Rel(5000ms)));
  g_apc_args[0] = g_apc_args[1] = g_apc_args[2] = 0;
  g_apc_thread = nullptr;
  const uint64_t before = rtl::QuerySystemTime();
  auto t = GuestThread::Create(kFnTimer, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(g_delay_status == kStatusUserApc);
  CHECK(g_apc_args[0] == 0x77);  // routine_arg
  const uint64_t fired = uint64_t(g_apc_args[2].load()) << 32 | g_apc_args[1].load();
  CHECK(fired >= before);  // (time_low, time_high) of the system time of the fire
  CHECK(g_apc_thread == t.get());
  CHECK(Call(__imp__NtClose, {g_timer}) == kStatusSuccess);
}

TEST_CASE("ExTerminateThread terminates immediately with the given code", "[threads]") {
  E();
  g_after = false;
  auto t = GuestThread::Create(kFnTerminate, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(t->ExitCode() == 0x55);
  CHECK_FALSE(g_after);
}

TEST_CASE("mutant owned by a terminating thread becomes abandoned", "[threads]") {
  E();
  g_mutant = std::make_shared<Mutant>(false);
  auto t = GuestThread::Create(kFnMutant, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  const std::shared_ptr<KernelObject> one[1] = {g_mutant};
  const int64_t zero = 0;
  CHECK(WaitAny(one, &zero, false) == kStatusAbandonedWait0);
  g_mutant.reset();
}

TEST_CASE("suspension: of itself immediately, of another at its next export", "[threads]") {
  auto& e = E();
  SECTION("KeSuspendThread on itself, resumed by NtResumeThread") {
    g_after = false;
    auto t = GuestThread::Create(kFnSelfSuspend, 0, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([&] { return Get8(t->KThreadAddr() + kKThreadSuspendCount) == 1; }));
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(g_after);
    const uint32_t handle = HandleCreate(t);
    const uint32_t count = e.Alloc(4);
    CHECK(Call(__imp__NtResumeThread, {handle, count}) == kStatusSuccess);
    CHECK(Get32(count) == 1);
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(g_after);
    CHECK(Call(__imp__NtClose, {handle}) == kStatusSuccess);
  }
  SECTION("NtSuspendThread on another thread: it stops at the next export") {
    g_release = false;
    g_loops = 0;
    auto t = GuestThread::Create(kFnYieldLoop, 0, 0, false);
    REQUIRE(t);
    REQUIRE(Eventually([] { return g_loops.load() > 10; }));
    const uint32_t handle = HandleCreate(t);
    const uint32_t count = e.Alloc(4);
    CHECK(Call(__imp__NtSuspendThread, {handle, count}) == kStatusSuccess);
    CHECK(Get32(count) == 0);
    // stopped when the counter does not change for 20 ms (under load the thread
    // may take longer to reach the next export)
    uint64_t frozen = 0;
    REQUIRE(Eventually([&] {
      const uint64_t before = g_loops;
      std::this_thread::sleep_for(20ms);
      frozen = g_loops;
      return frozen == before;
    }));
    std::this_thread::sleep_for(50ms);
    CHECK(g_loops <= frozen + 1);
    CHECK(Call(__imp__KeResumeThread, {t->KThreadAddr()}) == kStatusSuccess);
    REQUIRE(Eventually([&] { return g_loops.load() > frozen + 10; }));
    g_release = true;
    CHECK(WaitThread(t) == kStatusWait0);
    CHECK(Call(__imp__NtClose, {handle}) == kStatusSuccess);
  }
}

TEST_CASE("affinity, priority, KeGetCurrentProcessorNumber, XGetAVPack", "[threads]") {
  auto& e = E();
  g_release = false;
  auto t = GuestThread::Create(kFnCpu, 0, 0, false, 0, 0x02u << 24);
  REQUIRE(t);
  CHECK(t->Cpu() == 1);
  const uint32_t prev = e.Alloc(4);
  const uint32_t kt = t->KThreadAddr();
  CHECK(Call(__imp__KeSetAffinityThread, {kt, 0x10, prev}) == kStatusSuccess);
  CHECK(Get32(prev) == 0x2);
  CHECK(t->Cpu() == 4);
  CHECK(Get8(kt + kKThreadCurrentCpu) == 4);
  CHECK(Call(__imp__KeSetAffinityThread, {kt, 0, prev}) == kStatusInvalidParameter);
  CHECK(Call(__imp__KeSetBasePriorityThread, {kt, 0x10}) == 0);
  CHECK(Call(__imp__KeQueryBasePriorityThread, {kt}) == 0x10);
  CHECK(Call(__imp__KeSetBasePriorityThread, {kt, uint64_t(int64_t(-1))}) == 0x10);
  g_release = true;
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(g_cpu == 4);
  CHECK(Call(__imp__XGetAVPack, {}) == 6);
}

TEST_CASE("entry without a recompiled function: the thread terminates without crashing", "[threads]") {
  E();
  auto t = GuestThread::Create(kFnMissing, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(t->ExitCode() == kStatusUnsuccessful);
  // GuestToHostFunction from a host thread: no context, returns 0
  CHECK(GuestToHostFunction<uint32_t>(kFnPlusOne, 1u) == 0);
}

TEST_CASE("PCR/KTHREAD blocks and stack reused after the thread is destroyed", "[threads]") {
  E();
  auto t = GuestThread::Create(kFnNothing, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  const uint32_t pcr = t->PcrAddr();
  const uint32_t stack = t->GuardBase();
  const uint64_t used = BytesInUse();
  t.reset();
  // the last reference is the thread's own, released right after
  // waking whoever waits: destruction follows shortly after
  CHECK(Eventually([&] { return BytesInUse() == used - 0x40000; }));  // stack returned to the budget
  CHECK(ThreadFromKThread(pcr + kKThreadOffset) == nullptr);
  auto u = GuestThread::Create(kFnNothing, 0, 0, true);
  REQUIRE(u);
  CHECK(u->PcrAddr() == pcr);
  CHECK(Get32(u->KThreadAddr() + kKThreadExitStatus) == 0);  // block zeroed
  (void)stack;
  u->Resume();
  CHECK(WaitThread(u) == kStatusWait0);
}

TEST_CASE("CreateHost: host body on a guest thread, with GuestToHostFunction", "[threads]") {
  E();
  std::atomic<uint32_t> result{0}, r13{0};
  std::atomic<GuestThread*> self{nullptr};
  auto t = GuestThread::CreateHost(
      [&]() -> uint32_t {
        self = GuestThread::Current();
        r13 = GetPPCContext() ? GetPPCContext()->r13.u32 : 0;
        result = GuestToHostFunction<uint32_t>(kFnPlusOne, 41u);
        return 0x55;
      },
      0, "test host");
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(t->ExitCode() == 0x55);
  CHECK(self.load() == t.get());
  CHECK(r13.load() == t->PcrAddr());
  CHECK(result.load() == 42);
  CHECK(GuestThread::CreateHost({}, 0, "empty") == nullptr);
}

TEST_CASE("APCs queued to a terminated thread: dropped, one log line", "[threads]") {
  E();
  auto t = GuestThread::Create(kFnNothing, 0, 0, false);
  REQUIRE(t);
  REQUIRE(WaitThread(t) == kStatusWait0);
  REQUIRE(t->Exited());
  const auto path = gow2_test::UniqueTempPath("gow2_threads_apc");
  LogInit(path);
  for (int i = 0; i < 1000; ++i) t->QueueApc(kFnApc, uint32_t(i), 0, 0);
  const size_t pending = t->PendingApcs();
  LogInit({});  // closes the file
  CHECK(pending == 0);
  std::ifstream in(path);
  std::string line;
  int dropped_lines = 0;
  while (std::getline(in, line))
    if (line.find("[kernel] APC discarded") != std::string::npos) ++dropped_lines;
  in.close();
  std::filesystem::remove(path);
  CHECK(dropped_lines == 1);
}

TEST_CASE("KiApcNormalRoutineNop: empty routine, returns 0", "[threads]") {
  CHECK(Call(__imp__KiApcNormalRoutineNop, {0x1000, 0x13}) == 0);
}

TEST_CASE("handle 0xFFFFFFFE = current thread (Xenia ObjectTable::TranslateHandle)", "[threads]") {
  E();
  g_self_handle_ok = false;
  auto t = GuestThread::Create(kFnSelfHandle, 0, 0, false);
  REQUIRE(t);
  CHECK(WaitThread(t) == kStatusWait0);
  CHECK(g_self_handle_ok);
  CHECK(HandleGet(kCurrentThreadHandle) == nullptr);  // host thread
}

TEST_CASE("InterlockedPopEntrySList / InterlockedFlushSList on a big-endian X_SLIST_HEADER", "[threads]") {
  Env& e = E();
  const uint32_t list = e.mem.Alloc(0x40, 8);
  const uint32_t a = list + 0x10, b = list + 0x20;
  // list: header -> a -> b, depth 2, sequence 7 (as the game's inline push writes it)
  Put32(list, a);
  *GuestPtr<be<uint16_t>>(list + 4) = 2;
  *GuestPtr<be<uint16_t>>(list + 6) = 7;
  Put32(a, b);
  Put32(b, 0);
  CHECK(Call(__imp__InterlockedPopEntrySList, {list}) == a);
  CHECK(Get32(list) == b);
  CHECK(*GuestPtr<be<uint16_t>>(list + 4) == 1);
  CHECK(*GuestPtr<be<uint16_t>>(list + 6) == 7);
  CHECK(Call(__imp__InterlockedPopEntrySList, {list}) == b);
  CHECK(Get32(list) == 0);
  CHECK(Call(__imp__InterlockedPopEntrySList, {list}) == 0);  // empty
  // flush: returns the head and zeroes the header
  Put32(list, a);
  *GuestPtr<be<uint16_t>>(list + 4) = 2;
  Put32(a, b);
  CHECK(Call(__imp__InterlockedFlushSList, {list}) == a);
  CHECK(Get32(list) == 0);
  CHECK(Get32(list + 4) == 0);
}
