// Shim of Xenia's src/xenia/base/threading.cc and threading_win.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): standard library only.
#include "xenia/base/threading.h"

#include <atomic>
#include <thread>

namespace xe {
namespace threading {
namespace {

// Wait on a condition variable with an "infinite" timeout =
// milliseconds::max() (wait_for with that value would overflow).
template <typename Pred>
bool WaitUntil(std::condition_variable& cond, std::unique_lock<std::mutex>& lock,
               std::chrono::milliseconds timeout, Pred pred) {
  if (timeout == std::chrono::milliseconds::max()) {
    cond.wait(lock, pred);
    return true;
  }
  return cond.wait_until(lock, std::chrono::steady_clock::now() + timeout, pred);
}

class StdEvent final : public Event {
 public:
  StdEvent(bool manual_reset, bool initial_state)
      : manual_reset_(manual_reset), signaled_(initial_state) {}

  void Set() override {
    {
      std::lock_guard lock(mutex_);
      signaled_ = true;
    }
    if (manual_reset_) {
      cond_.notify_all();
    } else {
      cond_.notify_one();
    }
  }

  void Reset() override {
    std::lock_guard lock(mutex_);
    signaled_ = false;
  }

  void Pulse() override {
    {
      std::lock_guard lock(mutex_);
      ++pulse_generation_;
    }
    cond_.notify_all();
  }

 private:
  WaitResult WaitFor(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(mutex_);
    const uint64_t generation = pulse_generation_;
    if (!WaitUntil(cond_, lock, timeout,
                   [&] { return signaled_ || pulse_generation_ != generation; })) {
      return WaitResult::kTimeout;
    }
    if (signaled_ && !manual_reset_) {
      signaled_ = false;
    }
    return WaitResult::kSuccess;
  }

  const bool manual_reset_;
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_;
  uint64_t pulse_generation_ = 0;
};

thread_local Thread* t_current_thread = nullptr;

class StdThread final : public Thread {
 public:
  explicit StdThread(std::function<void()> start_routine) {
    thread_ = std::thread([this, routine = std::move(start_routine)] {
      t_current_thread = this;
      routine();
      t_current_thread = nullptr;
      {
        std::lock_guard lock(mutex_);
        finished_ = true;
      }
      cond_.notify_all();
    });
  }

  // The thread uses this (mutex_, cond_) until the end: it is joined. In
  // Xenia destroying the Thread only closes the handle; the GPU code always
  // destroys it after waiting for it.
  ~StdThread() override {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    uint32_t previous;
    {
      std::lock_guard lock(mutex_);
      previous = suspend_count_;
      if (suspend_count_) {
        --suspend_count_;
      }
    }
    cond_.notify_all();
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = previous;
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    if (t_current_thread != this) {
      return false;  // a std::thread cannot be stopped from outside
    }
    std::unique_lock lock(mutex_);
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    ++suspend_count_;
    cond_.wait(lock, [&] { return suspend_count_ == 0; });
    return true;
  }

 private:
  WaitResult WaitFor(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(mutex_);
    if (!WaitUntil(cond_, lock, timeout, [&] { return finished_; })) {
      return WaitResult::kTimeout;
    }
    return WaitResult::kSuccess;
  }

  std::mutex mutex_;
  std::condition_variable cond_;
  bool finished_ = false;
  uint32_t suspend_count_ = 0;
  std::thread thread_;
};

}  // namespace

void Fence::Signal() {
  {
    std::lock_guard lock(mutex_);
    signaled_ = true;
  }
  cond_.notify_all();
}

// Xenia (threading.h): every waiter is counted; on the signal all of them leave
// and the last one clears it, so no waiter already inside misses it.
void Fence::Wait() {
  std::unique_lock lock(mutex_);
  ++waiters_;
  cond_.wait(lock, [&] { return signaled_; });
  if (--waiters_ == 0) {
    signaled_ = false;
  }
}

uint32_t logical_processor_count() {
  const unsigned int count = std::thread::hardware_concurrency();
  return count ? count : 1;
}

void set_name(const std::string_view /*name*/) {}

void MaybeYield() { std::this_thread::yield(); }

void SyncMemory() { std::atomic_thread_fence(std::memory_order_seq_cst); }

void Sleep(std::chrono::microseconds duration) { std::this_thread::sleep_for(duration); }

WaitResult Wait(WaitHandle* wait_handle, bool /*is_alertable*/, std::chrono::milliseconds timeout) {
  if (!wait_handle) {
    return WaitResult::kFailed;
  }
  return wait_handle->WaitFor(timeout);
}

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<StdEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<StdEvent>(false, initial_state);
}

Thread* Thread::GetCurrentThread() { return t_current_thread; }

void gow2_SetCurrentThread(Thread* thread) { t_current_thread = thread; }

std::unique_ptr<Thread> Thread::Create(CreationParameters /*params*/,
                                       std::function<void()> start_routine) {
  return std::make_unique<StdThread>(std::move(start_routine));
}

}  // namespace threading
}  // namespace xe
