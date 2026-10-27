// Guest threads: each guest thread is a host thread (std::thread) with its own
// PPCContext, a guest stack with an inaccessible guard below it, a PCR and a
// KTHREAD in guest memory with Xenia's layout (X_KPCR / X_KTHREAD,
// src/xenia/kernel/xthread.h), pointed to by r13. The thread is a waitable
// object, signaled when it terminates.
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/kernel/xthread.cc (Create, InitializeGuestObject,
// AllocateStack, Execute, Exit, EnqueueApc, DeliverAPCs, SetActiveCpu).
//
// Guest layout per thread:
//   stack     virtual heap, reservation of kStackGuardSize + stack (rounded
//             to 64 KB, minimum 64 KB); the guard at the bottom stays reserved and
//             uncommitted, so every access fails (crash
//             classified "stack overflow"). r1 = top - 0x80.
//   block     runtime area (GuestMemory::Alloc), reused by new threads:
//             +0x0000 PCR (X_KPCR, 0x2D8)      TLS at +0, thread at +0x100,
//                                               CPU at +0x10C
//             +0x0800 TLS slots (kTlsSlots 4-byte entries)
//             +0x1000 KTHREAD (X_KTHREAD, 0xAB0) thread id at +0x14C
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// PPCFunc. Mind the order (see kernel/function.h): in gow2's TUs
// recomp_glue.h (generated ppc_config.h) must be included first.
#include "kernel/function.h"
#include "kernel/objects.h"

struct GuestMemory;

namespace kernel {

inline constexpr uint32_t kStackGuardSize = 0x10000;
inline constexpr uint32_t kMinStackSize = 0x10000;
inline constexpr uint32_t kTlsSlots = 64;  // GoW2's XEX_HEADER_TLS_INFO: 64 slots, no data
inline constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;
inline constexpr uint32_t kCreateSuspended = 0x1;
inline constexpr uint32_t kStatusNoMemory = 0xC0000017;

// Offsets in the per-thread block and in the structures (Xenia X_KPCR / X_KTHREAD).
inline constexpr uint32_t kThreadBlockSize = 0x2000;
inline constexpr uint32_t kTlsOffset = 0x800;
inline constexpr uint32_t kKThreadOffset = 0x1000;
inline constexpr uint32_t kPcrTlsPtr = 0x0;
inline constexpr uint32_t kPcrSelf = 0x30;
inline constexpr uint32_t kPcrStackBase = 0x70;
inline constexpr uint32_t kPcrStackEnd = 0x74;
inline constexpr uint32_t kPcrCurrentThread = 0x100;
inline constexpr uint32_t kPcrCurrentCpu = 0x10C;
inline constexpr uint32_t kPcrDpcActive = 0x150;
inline constexpr uint32_t kKThreadStackBase = 0x5C;
inline constexpr uint32_t kKThreadStackLimit = 0x60;
inline constexpr uint32_t kKThreadTls = 0x68;
inline constexpr uint32_t kKThreadProcess = 0x84;
inline constexpr uint32_t kKThreadSuspendCount = 0xBC;
inline constexpr uint32_t kKThreadCurrentCpu = 0xBF;
inline constexpr uint32_t kKThreadExitStatus = 0x140;
inline constexpr uint32_t kKThreadId = 0x14C;
inline constexpr uint32_t kKThreadStartAddress = 0x150;
inline constexpr uint32_t kKThreadCreationFlags = 0x16C;

// Recompiled function for a guest address (nullptr if there is none): in
// gow2.exe the function table (recomp_glue LookupFunction), in tests a
// fake table.
using GuestFunctionResolver = PPCFunc* (*)(uint32_t guest);

// Prepares the guest threads: runtime area for PCR/KTHREAD, function
// resolver, default stack (XEX_HEADER_DEFAULT_STACK_SIZE; 0 = 256 KB) and
// the dispatcher hooks (thread id = KTHREAD, AlertPoll, APC delivery,
// timer APCs). Must be called after InitMemory. false if memory is missing
// for the process block.
bool InitThreads(GuestMemory&, GuestFunctionResolver, uint32_t default_stack_size);
void SetGuestFunctionResolver(GuestFunctionResolver);
uint32_t DefaultStackSize();

class GuestThread final : public KernelObject {
 public:
  // stack_size 0 = DefaultStackSize(). With xapi_start != 0 the thread calls
  // xapi_start(entry, start_context) (Xenia ExCreateThread/XThread::Execute),
  // otherwise entry(start_context) and the return value is the exit
  // code. creation_flags as in ExCreateThread (bit 0 suspended, bits 24-31
  // affinity); startup_delay: 10 ms pause before the entry, like Xenia
  // XThread::Execute for threads created by the game. nullptr if memory is missing.
  static std::shared_ptr<GuestThread> Create(uint32_t entry, uint32_t start_context, uint32_t stack_size,
                                             bool suspended, uint32_t xapi_start = 0, uint32_t creation_flags = 0,
                                             bool startup_delay = false);
  // Guest thread with a host body (Xenia XHostThread, src/xenia/kernel/
  // xthread.cc): PCR, KTHREAD, TLS and guest stack like the others, but the thread
  // runs `body` (host code) instead of a guest function; the return value
  // is the exit code. Inside `body` GuestToHostFunction calls guest
  // code (fake GPU interrupt callbacks). `name` goes in the log and
  // in the host thread's name. nullptr if memory is missing.
  static std::shared_ptr<GuestThread> CreateHost(std::function<uint32_t()> body, uint32_t stack_size,
                                                 std::string name);
  ~GuestThread() override;

  // Suspend count: Resume decrements it (if > 0) and returns the
  // previous one; Suspend increments it and returns the previous one. A thread that
  // suspends itself stops immediately; another thread stops at its
  // next export (CheckSuspend in registry.h).
  uint32_t Resume();
  uint32_t Suspend();

  uint32_t ThreadId() const { return thread_id_; }
  uint32_t KThreadAddr() const { return block_ + kKThreadOffset; }
  uint32_t PcrAddr() const { return block_; }
  uint32_t TlsAddr() const { return block_ + kTlsOffset; }
  uint32_t StackBase() const { return stack_base_; }    // top (high address)
  uint32_t StackLimit() const { return stack_limit_; }  // bottom, right above the guard
  uint32_t GuardBase() const { return stack_alloc_; }

  // Current guest thread (nullptr on a host thread).
  static GuestThread* Current();

  // User APC (Xenia XThread::EnqueueApc): routine(normal_context, arg1,
  // arg2) run on the thread itself at the next alertable wait (which
  // returns STATUS_USER_APC). Thread-safe. On a terminated thread the APC is
  // dropped (the log says so once per thread: `[kernel] APC
  // discarded`), so a periodic timer does not grow the queue forever.
  void QueueApc(uint32_t routine, uint32_t normal_context, uint32_t arg1, uint32_t arg2);
  // The thread's next alertable wait returns STATUS_ALERTED.
  void Alert();
  // Runs the queued APCs; only on the thread itself.
  void DeliverApcs();
  size_t PendingApcs() const;

  // End of the thread from the thread itself (ExTerminateThread): does not return.
  [[noreturn]] void Exit(uint32_t code);
  bool Exited() const;
  uint32_t ExitCode() const { return exit_code_.load(); }

  // Recorded affinity and priority (KeSetAffinityThread,
  // KeSetBasePriorityThread); the priority is approximated on the host.
  // SetAffinity returns the previous mask (1 << cpu).
  uint32_t SetAffinity(uint32_t mask);
  uint8_t Cpu() const;
  int32_t SetPriority(int32_t increment);  // returns the previous one
  int32_t Priority() const { return priority_.load(); }

  // TLS (KeTlsGetValue/SetValue): false if slot >= kTlsSlots.
  bool GetTls(uint32_t slot, uint32_t* value) const;
  bool SetTls(uint32_t slot, uint32_t value);

  bool IsSignaled() const override { return exited_; }
  void Acquire() override {}
  uint8_t GuestType() const override { return kThreadObject; }

  // Parks the current thread while the suspend count is > 0.
  void ParkWhileSuspended();
  // First AlertPoll check for this thread (dispatcher lock held).
  uint32_t PollAlert();

 private:
  struct Apc {
    uint32_t routine, context, arg1, arg2;
  };
  GuestThread() = default;
  static std::shared_ptr<GuestThread> CreateImpl(uint32_t entry, uint32_t start_context, uint32_t stack_size,
                                                 bool suspended, uint32_t xapi_start, uint32_t creation_flags,
                                                 bool startup_delay, std::function<uint32_t()> host_body,
                                                 std::string host_name);
  void Run();
  void Finish(uint32_t code);
  void SetCpu(uint8_t cpu);

  uint32_t entry_ = 0, start_context_ = 0, xapi_start_ = 0, creation_flags_ = 0;
  bool startup_delay_ = false;
  std::function<uint32_t()> host_body_;  // CreateHost: host body in place of entry
  std::string host_name_;
  uint32_t thread_id_ = 0;
  uint32_t block_ = 0;
  uint32_t stack_alloc_ = 0, stack_limit_ = 0, stack_base_ = 0;
  uintptr_t guard_host_ = 0;

  std::thread host_;
  std::shared_ptr<GuestThread> self_;  // the thread owns itself while it runs
  bool exited_ = false;                // dispatcher lock
  std::atomic<uint32_t> exit_code_{0};
  std::atomic<int32_t> priority_{0};

  std::mutex suspend_mutex_;
  std::condition_variable resume_cv_;
  std::atomic<int32_t> suspend_count_{0};

  mutable std::mutex apc_mutex_;
  std::deque<Apc> apcs_;
  bool alerted_ = false;
  bool apcs_closed_ = false;      // Finish: APCs queued afterwards are dropped
  bool apc_drop_logged_ = false;  // drop log line already written
};

// Thread of the guest KTHREAD (nullptr if no live thread uses it).
std::shared_ptr<GuestThread> ThreadFromKThread(uint32_t kthread);

// Global TLS slots (Xenia KernelState::AllocateTLS/FreeTLS).
uint32_t GuestTlsAlloc();          // kTlsOutOfIndexes if exhausted
bool GuestTlsFree(uint32_t slot);  // zeroes the slot in all live threads

}  // namespace kernel
