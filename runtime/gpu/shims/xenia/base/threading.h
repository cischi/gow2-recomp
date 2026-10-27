// Shim of Xenia's src/xenia/base/threading.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): the part used by the GPU code (Fence, Event, Thread, Wait,
// Sleep, MaybeYield, SyncMemory, logical_processor_count), on std::thread,
// std::mutex and std::condition_variable. Xenia's "alertable" waits (host
// thread APCs) do not exist here: is_alertable is ignored.
#ifndef XENIA_BASE_THREADING_H_
#define XENIA_BASE_THREADING_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace xe {
namespace threading {

// Like Xenia: one Signal() releases all Wait() calls in progress (even if it
// arrives first); the last waiter to leave clears it.
class Fence {
 public:
  void Signal();
  void Wait();

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_ = false;
  uint32_t waiters_ = 0;  // waiters inside Wait()
};

uint32_t logical_processor_count();

void set_name(const std::string_view name);

void MaybeYield();
void SyncMemory();

void Sleep(std::chrono::microseconds duration);
template <typename Rep, typename Period>
void Sleep(std::chrono::duration<Rep, Period> duration) {
  Sleep(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

enum class WaitResult {
  kSuccess,
  kUserCallback,
  kTimeout,
  kAbandoned,
  kFailed,
};

class WaitHandle {
 public:
  virtual ~WaitHandle() = default;
  // No native handle: always nullptr.
  virtual void* native_handle() const { return nullptr; }

 protected:
  WaitHandle() = default;

 private:
  friend WaitResult Wait(WaitHandle*, bool, std::chrono::milliseconds);
  // Waits for the signaled state up to `timeout` (consuming it if auto-reset).
  virtual WaitResult WaitFor(std::chrono::milliseconds timeout) = 0;
};

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

class Event : public WaitHandle {
 public:
  // Stays signaled until Reset() is called.
  static std::unique_ptr<Event> CreateManualResetEvent(bool initial_state);
  // A successful wait returns it to non-signaled.
  static std::unique_ptr<Event> CreateAutoResetEvent(bool initial_state);

  virtual void Set() = 0;
  virtual void Reset() = 0;
  // Releases the current waiters and returns to non-signaled.
  virtual void Pulse() = 0;
  // xenia-canary: Set that on Windows immediately hands execution to the waiter.
  // Here like canary's non-Windows branch: Set.
  void SetBoostPriority() { Set(); }
};

struct ThreadPriority {
  static const int32_t kLowest = -2;
  static const int32_t kBelowNormal = -1;
  static const int32_t kNormal = 0;
  static const int32_t kAboveNormal = 1;
  static const int32_t kHighest = 2;
};

class Thread : public WaitHandle {
 public:
  struct CreationParameters {
    size_t stack_size = 4 * 1024 * 1024;
    bool create_suspended = false;
    int32_t initial_priority = 0;
  };

  // Runs `start_routine` on a std::thread; Wait() returns when it finishes.
  // create_suspended, stack_size and initial_priority are ignored.
  static std::unique_ptr<Thread> Create(CreationParameters params,
                                        std::function<void()> start_routine);

  // Current thread if created by Thread::Create or registered with
  // gow2_SetCurrentThread (XThread of the kernel shim); nullptr for other
  // threads (Xenia would create a handle for them).
  static Thread* GetCurrentThread();

  const std::string& name() const { return name_; }
  virtual void set_name(std::string name) { name_ = std::move(name); }

  // Suspend count like Xenia (ResumeThread/SuspendThread): Resume
  // decrements it (if > 0), Suspend increments it; the previous value goes to
  // *out_previous_suspend_count. A stopped thread resumes when it returns to 0.
  // On std::thread only self-suspension (Suspend from the thread itself, which
  // stops immediately); Suspend from another thread returns false.
  virtual bool Resume(uint32_t* out_previous_suspend_count = nullptr) = 0;
  virtual bool Suspend(uint32_t* out_previous_suspend_count = nullptr) = 0;

 protected:
  std::string name_;
};

// gow2: current thread for Thread::GetCurrentThread (thread_local), for
// threads not created by Thread::Create (XThread of the kernel shim).
void gow2_SetCurrentThread(Thread* thread);

}  // namespace threading
}  // namespace xe

#endif  // XENIA_BASE_THREADING_H_
