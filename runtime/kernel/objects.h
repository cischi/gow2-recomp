// Dispatcher kernel objects (events, semaphores, mutants, timers), handle
// table and guest address -> host object map.
//
// The state of the objects lives in the host objects and is protected by the
// dispatcher's single lock (sync.h). The guest structures (KEVENT & co.) serve
// to create the object the first time an address appears (initial state
// from the header) and to recognize it afterwards: like Xenia (XObject::GetNativeObject,
// src/xenia/kernel/xobject.cc) the header gets a signature in
// WaitListHead.Flink and an identifier in WaitListHead.Blink. If the game
// rewrites the header (inline KeInitialize*) the signature vanishes and the address
// gets a new object.
#pragma once

#include <xbox.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace kernel {

// Console NTSTATUS return codes.
inline constexpr uint32_t kStatusSuccess = 0;
inline constexpr uint32_t kStatusWait0 = 0;
inline constexpr uint32_t kStatusAbandonedWait0 = 0x80;
inline constexpr uint32_t kStatusUserApc = 0xC0;
inline constexpr uint32_t kStatusAlerted = 0x101;
inline constexpr uint32_t kStatusTimeout = 0x102;
inline constexpr uint32_t kStatusTimerResumeIgnored = 0x40000025;
inline constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
inline constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
inline constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
inline constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
inline constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
inline constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047;

// DISPATCHER_HEADER (Xenia X_DISPATCH_HEADER, src/xenia/kernel/xobject.h).
struct DispatcherHeader {
  uint8_t type;
  uint8_t absolute;  // for critical sections: spin count / 256
  uint8_t size;
  uint8_t inserted;
  be<int32_t> signal_state;
  be<uint32_t> wait_list_flink;
  be<uint32_t> wait_list_blink;
};
static_assert(sizeof(DispatcherHeader) == 16);

// DISPATCHER_HEADER.Type (KOBJECTS).
enum DispatcherType : uint8_t {
  kNotificationEvent = 0,
  kSynchronizationEvent = 1,
  kMutantObject = 2,
  kSemaphoreObject = 5,
  kThreadObject = 6,
  kNotificationTimer = 8,
  kSynchronizationTimer = 9,
};

namespace detail {
struct DispatcherAccess;
}  // namespace detail
class KernelObject;

namespace detail {
// Wait in progress of a thread (sync.cpp), registered in the objects.
struct Waiter {
  std::span<const std::shared_ptr<KernelObject>> objects;
  bool wait_all = false;
  uint32_t thread = 0;
  bool done = false;
  uint32_t status = 0;
};
}  // namespace detail

// Waitable object. IsSignaled/Acquire/IsAbandoned are only called with the
// dispatcher lock held and apply to the "acting thread"
// (ActingThreadId() in sync.h): the current thread, or the waiting thread
// on whose behalf the dispatcher is satisfying the wait.
//
// A subclass that becomes signaled must, with the lock held, call
// SatisfyWaiters(*this) (sync.h): that is how waiting threads receive
// the object without lost wakeups.
class KernelObject : public std::enable_shared_from_this<KernelObject> {
 public:
  KernelObject() = default;
  virtual ~KernelObject();
  KernelObject(const KernelObject&) = delete;
  KernelObject& operator=(const KernelObject&) = delete;

  virtual bool IsSignaled() const = 0;
  // Consumption on a successful wait (auto-reset event that resets, semaphore
  // count, mutant ownership...).
  virtual void Acquire() = 0;
  // true if the wait that acquires it must return STATUS_ABANDONED_WAIT_n.
  virtual bool IsAbandoned() const { return false; }
  // Type of the matching guest header (ObjectFromGuest, headers created for
  // ObReferenceObjectByHandle).
  virtual uint8_t GuestType() const = 0;

  // The object's guest structure (0 if it does not have one yet). Written under
  // the address map's lock.
  uint32_t guest_object() const { return guest_object_.load(); }

  // Threads waiting on this object (diagnostics and tests).
  size_t WaiterCount() const;

 private:
  friend struct detail::DispatcherAccess;
  std::vector<detail::Waiter*> waiters_;  // FIFO, protected by the dispatcher lock
  std::atomic<uint32_t> guest_object_{0};
  bool owns_guest_object_ = false;  // header allocated by EnsureGuestObject (map lock)
};

// Xenia XEvent (src/xenia/kernel/xevent.cc). The methods take the lock and
// return the previous state (0/1).
class Event final : public KernelObject {
 public:
  Event(bool manual_reset, bool initial_state);
  int32_t Set();
  int32_t Reset();
  int32_t Pulse();  // wakes current waiters, then not signaled
  int32_t SetLocked();  // like Set, with the dispatcher lock already held
  bool manual_reset() const { return manual_reset_; }

  bool IsSignaled() const override { return signaled_; }
  void Acquire() override;
  uint8_t GuestType() const override { return manual_reset_ ? kNotificationEvent : kSynchronizationEvent; }

 private:
  const bool manual_reset_;
  bool signaled_;
};

// Xenia XSemaphore (src/xenia/kernel/xsemaphore.cc).
class Semaphore final : public KernelObject {
 public:
  Semaphore(int32_t count, int32_t limit);
  // false (count unchanged) if count + adjustment exceeds the limit or
  // adjustment <= 0; *previous receives the previous count anyway.
  bool Release(int32_t adjustment, int32_t* previous);
  bool ReleaseLocked(int32_t adjustment, int32_t* previous);  // lock held
  int32_t limit() const { return limit_; }

  bool IsSignaled() const override { return count_ > 0; }
  void Acquire() override { --count_; }
  uint8_t GuestType() const override { return kSemaphoreObject; }

 private:
  int32_t count_;
  const int32_t limit_;
};

// Xenia XMutant (src/xenia/kernel/xmutant.cc), with the NT semantics of
// KMUTANT: SignalState 1 = free, each recursive acquisition decrements it.
class Mutant final : public KernelObject {
 public:
  explicit Mutant(bool initial_owner);
  ~Mutant() override;
  // STATUS_MUTANT_NOT_OWNED if the current thread does not own it (and not
  // abandon). *previous = previous SignalState. abandon: full release
  // and the next acquisition sees STATUS_ABANDONED.
  uint32_t Release(bool abandon, int32_t* previous);
  uint32_t ReleaseLocked(bool abandon, int32_t* previous);  // lock held
  uint32_t Owner() const;  // id of the owning thread, 0 if free
  int32_t SignalState() const;

  bool IsSignaled() const override;
  void Acquire() override;
  bool IsAbandoned() const override { return abandoned_; }
  uint8_t GuestType() const override { return kMutantObject; }

 private:
  friend void AbandonMutantsOwnedBy(uint32_t thread_id);
  int32_t signal_state_ = 1;
  uint32_t owner_ = 0;
  bool abandoned_ = false;
};

// Mutants still owned by `thread_id` become free and abandoned
// (called when a guest thread exits, threads.cpp).
void AbandonMutantsOwnedBy(uint32_t thread_id);

// Timer (Xenia XTimer, src/xenia/kernel/xtimer.cc). Firing is handled by
// a dispatcher service thread (sync.cpp).
class Timer final : public KernelObject {
 public:
  explicit Timer(bool synchronization);
  ~Timer() override;
  // due: Xbox value (100 ns; negative = relative, positive = absolute
  // FILETIME). period_ms 0 = one-shot. routine/arg: APC to queue to the
  // thread that set the timer on every fire (SetTimerApcHook, sync.h).
  // The state goes back to not signaled. Returns true if the timer was already active.
  bool Set(int64_t due, uint32_t period_ms, uint32_t routine, uint32_t routine_arg);
  bool Cancel();  // true if it was active; the signaled state does not change
  bool SignalState() const;

  bool IsSignaled() const override { return signaled_; }
  void Acquire() override {
    if (synchronization_) signaled_ = false;
  }
  uint8_t GuestType() const override { return synchronization_ ? kSynchronizationTimer : kNotificationTimer; }

 private:
  friend struct detail::DispatcherAccess;
  const bool synchronization_;
  bool signaled_ = false;
  bool active_ = false;
  int64_t due_ns_ = 0;  // steady_clock, nanoseconds since the epoch
  int64_t period_ns_ = 0;
  uint32_t routine_ = 0, routine_arg_ = 0, apc_thread_ = 0;
};

namespace detail {
// Dispatcher access (sync.cpp, objects.cpp) to the internal fields. With the
// dispatcher lock held, except where noted.
struct DispatcherAccess {
  static std::vector<Waiter*>& Waiters(KernelObject& o) { return o.waiters_; }
  // address map lock
  static std::atomic<uint32_t>& GuestObject(KernelObject& o) { return o.guest_object_; }
  static bool& OwnsGuestObject(KernelObject& o) { return o.owns_guest_object_; }
  // timer: state for the service thread
  struct TimerState {
    bool& signaled;
    bool& active;
    int64_t& due_ns;
    int64_t& period_ns;
    uint32_t routine, routine_arg, apc_thread;
  };
  static TimerState State(Timer& t) {
    return {t.signaled_, t.active_, t.due_ns_, t.period_ns_, t.routine_, t.routine_arg_, t.apc_thread_};
  }
};
}  // namespace detail

// ---- guest address -> object map -----------------------------------------

// Object of the guest header at `guest_header`: the one already associated if the
// signature is intact and the type matches, otherwise a new one of the type given
// by DISPATCHER_HEADER.Type with the header's initial state (unknown
// threads/types: nullptr, logged). Critical sections (type 1 headers)
// use the synchronization event.
std::shared_ptr<KernelObject> ObjectFromGuest(uint32_t guest_header);
// Forgets the object associated with guest_header and clears the signature: the
// KeInitialize* functions call it after writing the new header.
void ResetGuestObject(uint32_t guest_header);
// Associates an existing object with the guest structure `guest_header` (e.g. a thread's
// KTHREAD, threads.cpp). The map only keeps a weak reference:
// the object lives as long as handles or references hold it.
void BindGuestObject(uint32_t guest_header, const std::shared_ptr<KernelObject>&);
// The object's guest structure; if it has none, allocates a DISPATCHER_HEADER
// in the process heap and associates it (ObReferenceObjectByHandle). 0 if
// there is no memory.
uint32_t EnsureGuestObject(const std::shared_ptr<KernelObject>&);

// ---- handle table --------------------------------------------------------

// Handle != 0 (multiples of 4, never 0xFFFFFFFF/0xFFFFFFFE). Each handle holds a
// reference to the object. Thread-safe.
// Pseudo-handle of the current thread (NtCurrentThread): HandleGet translates it
// to the calling GuestThread's object (nullptr on a host thread).
inline constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
std::shared_ptr<KernelObject> CurrentThreadObject();  // threads.cpp
uint32_t HandleCreate(std::shared_ptr<KernelObject>);
std::shared_ptr<KernelObject> HandleGet(uint32_t handle);  // nullptr if unknown
bool HandleClose(uint32_t handle);

// References of ObReferenceObjectByHandle/ObDereferenceObject, per
// guest structure. Thread-safe.
void ObjectReference(uint32_t guest_object, std::shared_ptr<KernelObject>);
bool ObjectDereference(uint32_t guest_object);

}  // namespace kernel
