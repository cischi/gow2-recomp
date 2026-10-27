// Guest threads (see threads.h).
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/kernel/xthread.cc (XThread::Create, InitializeGuestObject,
// AllocateStack, Execute, Exit, EnqueueApc, DeliverAPCs, SetActiveCpu,
// GetFakeCpuNumber, SetPriority) and kernel_state.cc (AllocateTLS, FreeTLS,
// process information block). The per-thread PPCContext and
// the start with r1/r13 follow UnleashedRecomp cpu/guest_thread.cpp.
#include "kernel/threads.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "crash_map.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "kernel/rtl_exports.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "log.h"
#include "platform/platform.h"

namespace kernel {
namespace {

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

void W8(uint32_t addr, uint8_t v) { *GuestPtr<uint8_t>(addr) = v; }
void W32(uint32_t addr, uint32_t v) { *GuestPtr<be<uint32_t>>(addr) = v; }
void W64(uint32_t addr, uint64_t v) { *GuestPtr<be<uint64_t>>(addr) = v; }
uint32_t R32(uint32_t addr) { return *GuestPtr<be<uint32_t>>(addr); }

// Global state never destroyed (threads may survive until exit).
struct State {
  GuestMemory* mem = nullptr;
  std::atomic<GuestFunctionResolver> resolver{nullptr};
  std::atomic<uint32_t> default_stack{0x40000};
  std::atomic<uint32_t> process_block{0};
  std::atomic<uint32_t> next_thread_id{0};
  std::atomic<uint32_t> next_cpu{0};

  // KTHREAD -> thread registry, free PCR/KTHREAD blocks, TLS slots.
  // No shared_ptr is released while this lock is held.
  std::mutex mutex;
  std::unordered_map<uint32_t, std::weak_ptr<GuestThread>> by_kthread;
  std::vector<uint32_t> free_blocks;
  uint64_t tls_bitmap = 0;
};

State& S() {
  static State* s = new State;
  return *s;
}

thread_local GuestThread* t_current = nullptr;

// Zeroed PCR + TLS + KTHREAD block: reused from a destroyed thread or
// new from the runtime area. 0 if the space is exhausted.
uint32_t AllocBlock() {
  State& s = S();
  uint32_t block = 0;
  {
    std::lock_guard g(s.mutex);
    if (!s.free_blocks.empty()) {
      block = s.free_blocks.back();
      s.free_blocks.pop_back();
    } else if (s.mem) {
      block = s.mem->Alloc(kThreadBlockSize, 0x1000);
    }
  }
  if (block) std::memset(GuestPtr<uint8_t>(block), 0, kThreadBlockSize);
  return block;
}

// Xenia GetFakeCpuNumber: mask 0 = next CPU in rotation, otherwise
// the highest bit (six hardware threads).
uint8_t FakeCpuNumber(uint32_t mask) {
  mask &= 0xFF;
  if (!mask) return uint8_t(S().next_cpu.fetch_add(1) % 6);
  return uint8_t((31 - std::countl_zero(mask)) % 6);
}

// Xenia KernelState: ProcessInfoBlock (0x60 bytes), pointed to by KTHREAD+0x84.
uint32_t MakeProcessBlock(GuestMemory& mem) {
  const uint32_t pib = mem.Alloc(0x60, 0x10);
  if (!pib) return 0;
  std::memset(GuestPtr<uint8_t>(pib), 0, 0x60);
  W32(pib + 0x0C, 0x0000007F);
  W32(pib + 0x10, 0x001F0000);
  W8(pib + 0x1B, 0x06);
  W32(pib + 0x1C, 16 * 1024);                       // kernel_stack_size
  *GuestPtr<be<uint16_t>>(pib + 0x2C) = kTlsSlots * 4;  // tls_slot_size
  W8(pib + 0x2F, 1);                                // process_type: X_PROCTYPE_USER
  return pib;
}

// ---- dispatcher hooks ------------------------------------------------------

uint32_t CurrentKThread() { return t_current ? t_current->KThreadAddr() : 0; }

uint32_t AlertPollHook(uint32_t thread_id) {
  GuestThread* t = t_current;
  if (!t || t->KThreadAddr() != thread_id) return 0;
  return t->PollAlert();
}

void DeliverApcsHook() {
  if (GuestThread* t = t_current) t->DeliverApcs();
}

// Xenia XTimer: the routine receives (routine_arg, time_low, time_high) as an APC
// of the thread that set the timer.
void TimerApcHook(uint32_t thread_id, uint32_t routine, uint32_t routine_arg, uint64_t system_time) {
  if (auto t = ThreadFromKThread(thread_id)) {
    t->QueueApc(routine, routine_arg, uint32_t(system_time), uint32_t(system_time >> 32));
    return;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 20)
    LogLine("[kernel] APC routine " + Hex(routine) + " of a timer set by a non-guest or terminated thread: ignored");
}

}  // namespace

// ---- configuration ---------------------------------------------------------------

bool InitThreads(GuestMemory& mem, GuestFunctionResolver resolver, uint32_t default_stack_size) {
  State& s = S();
  {
    std::lock_guard g(s.mutex);
    s.mem = &mem;
  }
  s.resolver = resolver;
  s.default_stack = default_stack_size ? default_stack_size : 0x40000;
  const uint32_t pib = MakeProcessBlock(mem);
  if (!pib) return false;
  s.process_block = pib;
  SetThreadIdProvider(&CurrentKThread);
  SetAlertPoll(&AlertPollHook);
  SetApcDelivery(&DeliverApcsHook);
  SetTimerApcHook(&TimerApcHook);
  return true;
}

void SetGuestFunctionResolver(GuestFunctionResolver resolver) { S().resolver = resolver; }

uint32_t DefaultStackSize() { return S().default_stack.load(); }

PPCFunc* FindGuestFunction(uint32_t guest) {
  const GuestFunctionResolver r = S().resolver.load();
  return r ? r(guest) : nullptr;
}

namespace detail {
void LogGuestCallFailure(uint32_t guest, const char* why) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 50) LogLine("[kernel] guest call to " + Hex(guest) + " not executed: " + why);
}
}  // namespace detail

void SuspendSafePoint() {
  if (GuestThread* t = t_current) t->ParkWhileSuspended();
}

// ---- GuestThread ------------------------------------------------------------------

// Xenia XThread::Create + AllocateStack + InitializeGuestObject.
std::shared_ptr<GuestThread> GuestThread::Create(uint32_t entry, uint32_t start_context, uint32_t stack_size,
                                                 bool suspended, uint32_t xapi_start, uint32_t creation_flags,
                                                 bool startup_delay) {
  return CreateImpl(entry, start_context, stack_size, suspended, xapi_start, creation_flags, startup_delay, {}, {});
}

// Xenia XHostThread: same guest object, host body.
std::shared_ptr<GuestThread> GuestThread::CreateHost(std::function<uint32_t()> body, uint32_t stack_size,
                                                     std::string name) {
  if (!body) return nullptr;
  return CreateImpl(0, 0, stack_size, false, 0, 0, false, std::move(body), std::move(name));
}

std::shared_ptr<GuestThread> GuestThread::CreateImpl(uint32_t entry, uint32_t start_context, uint32_t stack_size,
                                                     bool suspended, uint32_t xapi_start, uint32_t creation_flags,
                                                     bool startup_delay, std::function<uint32_t()> host_body,
                                                     std::string host_name) {
  State& s = S();
  MemorySystem* m = Memory();
  if (!m || !s.mem) {
    LogLine("[kernel] GuestThread::Create before InitMemory/InitThreads");
    return nullptr;
  }
  std::shared_ptr<GuestThread> t(new GuestThread());
  t->entry_ = entry;
  t->start_context_ = start_context;
  t->xapi_start_ = xapi_start;
  t->creation_flags_ = creation_flags;
  t->startup_delay_ = startup_delay;
  t->host_body_ = std::move(host_body);
  t->host_name_ = std::move(host_name);
  t->thread_id_ = s.next_thread_id.fetch_add(1) + 1;

  // Stack: size rounded to 64 KB (minimum 64 KB) with the guard below,
  // in a single reservation of the virtual heap; only the stack is committed.
  if (!stack_size) stack_size = DefaultStackSize();
  if (stack_size > 0x10000000) return nullptr;  // 256 MB: anything above is absurd
  const uint32_t size = std::max(kMinStackSize, (stack_size + 0xFFFF) & ~0xFFFFu);
  const uint32_t alloc = m->virt.Allocate(0, kStackGuardSize + size, kMemReserve | kMemTopDown, kPageReadWrite);
  if (!alloc) return nullptr;
  t->stack_alloc_ = alloc;
  t->stack_limit_ = alloc + kStackGuardSize;
  t->stack_base_ = t->stack_limit_ + size;
  if (!m->virt.Allocate(t->stack_limit_, size, kMemCommit, kPageReadWrite)) return nullptr;

  t->block_ = AllocBlock();
  if (!t->block_) return nullptr;

  const uint32_t pcr = t->PcrAddr(), kt = t->KThreadAddr(), tls = t->TlsAddr();
  // X_KPCR
  W32(pcr + kPcrTlsPtr, tls);
  W32(pcr + kPcrSelf, pcr);
  W32(pcr + kPcrStackBase, t->stack_base_);
  W32(pcr + kPcrStackEnd, t->stack_limit_);
  W32(pcr + kPcrCurrentThread, kt);
  W32(pcr + kPcrDpcActive, 0);
  // X_KTHREAD (Xenia InitializeGuestObject)
  W8(kt + 0x00, kThreadObject);
  W8(kt + kKThreadSuspendCount, suspended ? 1 : 0);
  W32(kt + 0x010, kt + 0x010);
  W32(kt + 0x014, kt + 0x010);
  W32(kt + 0x040, kt + 0x018 + 8);
  W32(kt + 0x044, kt + 0x018 + 8);
  W32(kt + 0x048, kt);
  W32(kt + 0x04C, kt + 0x018);
  *GuestPtr<be<uint16_t>>(kt + 0x054) = 0x102;
  *GuestPtr<be<uint16_t>>(kt + 0x056) = 1;
  W32(kt + kKThreadStackBase, t->stack_base_);
  W32(kt + kKThreadStackLimit, t->stack_limit_);
  W32(kt + kKThreadTls, tls);
  W32(kt + 0x074, kt + 0x074);
  W32(kt + 0x078, kt + 0x074);
  W32(kt + 0x07C, kt + 0x07C);
  W32(kt + 0x080, kt + 0x07C);
  W32(kt + kKThreadProcess, s.process_block.load());
  W8(kt + 0x08B, 1);
  W32(kt + 0x09C, 0xFDFFD7FF);
  W32(kt + 0x0D0, t->stack_base_);
  W64(kt + 0x130, rtl::QuerySystemTime());
  W32(kt + 0x144, kt + 0x144);
  W32(kt + 0x148, kt + 0x144);
  W32(kt + kKThreadId, t->thread_id_);
  W32(kt + kKThreadStartAddress, entry);
  W32(kt + 0x154, kt + 0x154);
  W32(kt + 0x158, kt + 0x154);
  W32(kt + 0x160, 0);  // last error
  W32(kt + kKThreadCreationFlags, creation_flags);
  W32(kt + 0x17C, 1);
  t->SetCpu(FakeCpuNumber(creation_flags >> 24));
  // Xenia XThread::Create: bits 0x20/0x40 of the flags = initial priority.
  if (creation_flags & 0x60) t->priority_ = (creation_flags & 0x20) ? 1 : 0;

  // The guest KTHREAD and the thread are the same object for waits.
  BindGuestObject(kt, t);
  {
    std::lock_guard g(s.mutex);
    s.by_kthread[kt] = t;
  }
  const uintptr_t guard = reinterpret_cast<uintptr_t>(GuestPtr<uint8_t>(alloc));
  if (RegisterStackGuard(guard, guard + kStackGuardSize)) {
    t->guard_host_ = guard;
  } else {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) LogLine("[kernel] stack guard table full: overflows not classified");
  }

  t->suspend_count_ = suspended ? 1 : 0;
  t->self_ = t;
  try {
    GuestThread* raw = t.get();
    t->host_ = std::thread([raw] { raw->Run(); });
  } catch (...) {
    t->self_.reset();
    LogLine("[kernel] cannot create the host thread for guest thread " + Hex(entry));
    return nullptr;
  }
  stats::NoteThreadCreated();
  LogLine("[kernel] thread " + std::to_string(t->thread_id_) + " created: " +
          (t->host_body_ ? "host \"" + t->host_name_ + "\"" : "entry " + Hex(entry)) +
          (xapi_start ? " via " + Hex(xapi_start) : std::string()) + ", context " + Hex(start_context) +
          ", stack " + Hex(t->stack_limit_) + "-" + Hex(t->stack_base_) + ", KTHREAD " + Hex(kt) + ", PCR " +
          Hex(pcr) + (suspended ? ", suspended" : ""));
  return t;
}

GuestThread::~GuestThread() {
  if (host_.joinable()) {
    if (host_.get_id() == std::this_thread::get_id())
      host_.detach();  // last reference released by the thread itself, at the end
    else
      host_.join();
  }
  State& s = S();
  if (block_) {
    std::lock_guard g(s.mutex);
    if (auto it = s.by_kthread.find(KThreadAddr()); it != s.by_kthread.end() && it->second.expired())
      s.by_kthread.erase(it);
  }
  if (guard_host_) UnregisterStackGuard(guard_host_);
  if (stack_alloc_)
    if (MemorySystem* m = Memory()) m->virt.Free(stack_alloc_, 0, kMemRelease);
  if (block_) {
    std::lock_guard g(s.mutex);
    s.free_blocks.push_back(block_);
  }
}

GuestThread* GuestThread::Current() { return t_current; }

// Xenia XThread::Execute: APCs already queued, then xapi_start(entry, context)
// or entry(context), whose r3 is the exit code.
void GuestThread::Run() {
  PPCContext ctx{};
  ctx.fpscr.loadFromHost();
  ctx.r1.u64 = stack_base_ - 0x80;
  ctx.r13.u64 = PcrAddr();
  t_current = this;
  t_suspend_count = &suspend_count_;
  SetPPCContext(&ctx);
  plat::SetCurrentThreadName("GuestThread " + std::to_string(thread_id_) + " " +
                             (host_body_ ? host_name_ : Hex(entry_)));

  ParkWhileSuspended();
  // Xenia: mandatory pause for games that initialize shared structures
  // after CreateThread, counting on the console's slowness.
  if (startup_delay_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  DeliverApcs();

  uint32_t code = 0;
  const uint32_t target = xapi_start_ ? xapi_start_ : entry_;
  if (host_body_) {
    code = host_body_();
  } else if (PPCFunc* fn = FindGuestFunction(target)) {
    if (xapi_start_) {
      ctx.r3.u64 = entry_;
      ctx.r4.u64 = start_context_;
    } else {
      ctx.r3.u64 = start_context_;
    }
    fn(ctx, g_base);
    code = xapi_start_ ? 0 : ctx.r3.u32;
  } else {
    LogLine("[kernel] thread " + std::to_string(thread_id_) + ": " + Hex(target) + " has no recompiled function");
    code = kStatusUnsuccessful;
  }
  Finish(code);
}

// Xenia XThread::Exit: APCs dropped, mutants abandoned, KTHREAD signaled
// with the exit code, waits satisfied.
void GuestThread::Finish(uint32_t code) {
  {
    std::lock_guard g(apc_mutex_);
    apcs_.clear();
    apcs_closed_ = true;
  }
  const uint32_t kt = KThreadAddr();
  AbandonMutantsOwnedBy(kt);
  W32(kt + kKThreadExitStatus, code);
  W64(kt + 0x138, rtl::QuerySystemTime());
  GuestPtr<DispatcherHeader>(kt)->signal_state = 1;
  exit_code_ = code;
  {
    auto lock = LockDispatcher();
    exited_ = true;
    SatisfyWaiters(*this);
  }
  LogLine("[kernel] thread " + std::to_string(thread_id_) + " terminated, code " + Hex(code));
  t_current = nullptr;
  t_suspend_count = nullptr;
  SetPPCContext(nullptr);
  // Last step: may destroy *this.
  std::shared_ptr<GuestThread> self = std::move(self_);
  self.reset();
}

void GuestThread::Exit(uint32_t code) {
  Finish(code);
  plat::ExitCurrentThread(code);
}

bool GuestThread::Exited() const {
  auto lock = LockDispatcher();
  return exited_;
}

uint32_t GuestThread::Resume() {
  std::lock_guard g(suspend_mutex_);
  const int32_t prev = suspend_count_.load();
  if (prev > 0) {
    suspend_count_ = prev - 1;
    W8(KThreadAddr() + kKThreadSuspendCount, uint8_t(prev - 1));
    if (prev == 1) resume_cv_.notify_all();
  }
  return uint32_t(prev);
}

uint32_t GuestThread::Suspend() {
  int32_t prev;
  {
    std::lock_guard g(suspend_mutex_);
    prev = suspend_count_.fetch_add(1);
    W8(KThreadAddr() + kKThreadSuspendCount, uint8_t(prev + 1));
  }
  if (t_current == this) ParkWhileSuspended();
  return uint32_t(prev);
}

void GuestThread::ParkWhileSuspended() {
  std::unique_lock lock(suspend_mutex_);
  resume_cv_.wait(lock, [this] { return suspend_count_.load() <= 0; });
}

void GuestThread::QueueApc(uint32_t routine, uint32_t normal_context, uint32_t arg1, uint32_t arg2) {
  bool dropped = false, log_drop = false;
  {
    std::lock_guard g(apc_mutex_);
    if (apcs_closed_) {
      dropped = true;
      log_drop = !apc_drop_logged_;
      apc_drop_logged_ = true;
    } else {
      apcs_.push_back({routine, normal_context, arg1, arg2});
    }
  }
  if (dropped) {
    // outside the lock: LogLine takes its own
    if (log_drop)
      LogLine("[kernel] APC discarded: thread " + std::to_string(thread_id_) + " terminated (routine " + Hex(routine) +
              "; later ones are not logged)");
    return;
  }
  WakeWaiters();
}

void GuestThread::Alert() {
  {
    std::lock_guard g(apc_mutex_);
    alerted_ = true;
  }
  WakeWaiters();
}

// Dispatcher lock held, on the thread itself. Like Xenia DeliverAPCs, the
// APCs do not run in a critical region (KeEnterCriticalRegion).
uint32_t GuestThread::PollAlert() {
  std::lock_guard g(apc_mutex_);
  if (!apcs_.empty() && CriticalRegionDepth() == 0) return kStatusUserApc;
  if (alerted_) {
    alerted_ = false;
    return kStatusAlerted;
  }
  return 0;
}

// Xenia XThread::DeliverAPCs (normal routines only: this runtime's APCs
// have no kernel routine).
void GuestThread::DeliverApcs() {
  while (CriticalRegionDepth() == 0) {
    Apc apc;
    {
      std::lock_guard g(apc_mutex_);
      if (apcs_.empty()) return;
      apc = apcs_.front();
      apcs_.pop_front();
    }
    GuestToHostFunction<void>(apc.routine, apc.context, apc.arg1, apc.arg2);
  }
}

size_t GuestThread::PendingApcs() const {
  std::lock_guard g(apc_mutex_);
  return apcs_.size();
}

void GuestThread::SetCpu(uint8_t cpu) {
  W8(PcrAddr() + kPcrCurrentCpu, cpu);
  W8(KThreadAddr() + kKThreadCurrentCpu, cpu);
}

uint8_t GuestThread::Cpu() const { return *GuestPtr<uint8_t>(PcrAddr() + kPcrCurrentCpu); }

// Xenia XThread::SetAffinity -> SetActiveCpu. The host affinity is not
// touched (approximation: the host scheduler distributes the threads).
uint32_t GuestThread::SetAffinity(uint32_t mask) {
  const uint32_t prev = 1u << Cpu();
  SetCpu(FakeCpuNumber(mask));
  return prev;
}

// Xenia XThread::SetPriority: increment recorded, host priority by bands.
int32_t GuestThread::SetPriority(int32_t increment) {
  const int32_t prev = priority_.exchange(increment);
  int level = 0;
  if (increment > 0x22)
    level = 2;
  else if (increment > 0x11)
    level = 1;
  else if (increment < -0x22)
    level = -2;
  else if (increment < -0x11)
    level = -1;
  if (host_.joinable()) plat::SetThreadPriority(host_.native_handle(), level);
  return prev;
}

bool GuestThread::GetTls(uint32_t slot, uint32_t* value) const {
  if (slot >= kTlsSlots) return false;
  *value = R32(TlsAddr() + slot * 4);
  return true;
}

bool GuestThread::SetTls(uint32_t slot, uint32_t value) {
  if (slot >= kTlsSlots) return false;
  W32(TlsAddr() + slot * 4, value);
  return true;
}

// ---- global registry and TLS -----------------------------------------------------

std::shared_ptr<GuestThread> ThreadFromKThread(uint32_t kthread) {
  State& s = S();
  std::shared_ptr<GuestThread> t;
  std::lock_guard g(s.mutex);
  if (auto it = s.by_kthread.find(kthread); it != s.by_kthread.end()) t = it->second.lock();
  return t;
}

// NtCurrentThread pseudo-handle (0xFFFFFFFE) of HandleGet (objects.h).
std::shared_ptr<KernelObject> CurrentThreadObject() {
  GuestThread* t = GuestThread::Current();
  return t ? ThreadFromKThread(t->KThreadAddr()) : nullptr;
}

uint32_t GuestTlsAlloc() {
  State& s = S();
  std::lock_guard g(s.mutex);
  if (~s.tls_bitmap == 0) return kTlsOutOfIndexes;
  const uint32_t slot = uint32_t(std::countr_one(s.tls_bitmap));
  s.tls_bitmap |= uint64_t(1) << slot;
  return slot;
}

bool GuestTlsFree(uint32_t slot) {
  if (slot >= kTlsSlots) return false;
  State& s = S();
  std::vector<std::shared_ptr<GuestThread>> threads;  // released after the lock
  {
    std::lock_guard g(s.mutex);
    s.tls_bitmap &= ~(uint64_t(1) << slot);
    for (auto& [kt, weak] : s.by_kthread)
      if (auto t = weak.lock()) threads.push_back(std::move(t));
  }
  for (auto& t : threads) t->SetTls(slot, 0);
  return true;
}

}  // namespace kernel
