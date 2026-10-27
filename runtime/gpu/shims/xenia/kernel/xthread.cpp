// Shim of Xenia's src/xenia/kernel/xthread.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): XThread/XHostThread on kernel::GuestThread::CreateHost (see
// xthread.h). Builds with the runtime (library gow2_gpu_xenia, C++23).
#include "xenia/kernel/xthread.h"

// First among the kernel headers: kernel/function.h (see kernel/threads.h).
#include "kernel/function.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "kernel/objects.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace {

thread_local XThread* t_current_xthread = nullptr;

constexpr uint32_t kStatusSuccess = 0;

// Waits for a guest thread to finish (NT: negative relative timeout in 100 ns units).
bool WaitGuestThread(const std::shared_ptr<::kernel::GuestThread>& guest, const int64_t* timeout) {
  const std::shared_ptr<::kernel::KernelObject> one[1] = {guest};
  return ::kernel::WaitAny(one, timeout, false) == 0;  // STATUS_WAIT_0
}

}  // namespace

// Threading handle of the guest thread: Suspend/Resume go to GuestThread's
// suspend count (a thread that suspends itself stops immediately; one
// suspended from outside stops at the next export, as in gow2).
class XThread::Handle final : public xe::threading::Thread {
 public:
  explicit Handle(std::shared_ptr<::kernel::GuestThread> guest) : guest_(std::move(guest)) {}

  bool Resume(uint32_t* out_previous_suspend_count) override {
    const uint32_t previous = guest_->Resume();
    if (out_previous_suspend_count) *out_previous_suspend_count = previous;
    return true;
  }
  bool Suspend(uint32_t* out_previous_suspend_count) override {
    const uint32_t previous = guest_->Suspend();
    if (out_previous_suspend_count) *out_previous_suspend_count = previous;
    return true;
  }

 private:
  xe::threading::WaitResult WaitFor(std::chrono::milliseconds timeout) override {
    if (timeout == std::chrono::milliseconds::max()) {
      return WaitGuestThread(guest_, nullptr) ? xe::threading::WaitResult::kSuccess
                                              : xe::threading::WaitResult::kFailed;
    }
    const int64_t relative = -int64_t(timeout.count()) * 10000;
    return WaitGuestThread(guest_, &relative) ? xe::threading::WaitResult::kSuccess
                                              : xe::threading::WaitResult::kTimeout;
  }

  std::shared_ptr<::kernel::GuestThread> guest_;
};

XThread::XThread(KernelState* /*kernel_state*/, uint32_t stack_size) : stack_size_(stack_size) {}

// The thread has already finished (Wait) or never started: GuestThread's
// destructor joins.
XThread::~XThread() = default;

bool XThread::IsInThread(XThread* other) { return t_current_xthread == other; }
bool XThread::IsInThread() { return t_current_xthread != nullptr; }

uint32_t XThread::Create() {
  if (guest_) return kStatusSuccess;
  // CreateHost starts the thread immediately, but the handle (thread_, for
  // Thread::GetCurrentThread) can only be created afterwards: the body waits for it.
  auto ready = std::make_shared<std::atomic<xe::threading::Thread*>>(nullptr);
  guest_ = ::kernel::GuestThread::CreateHost(
      [this, ready]() -> uint32_t {
        xe::threading::Thread* handle;
        while (!(handle = ready->load(std::memory_order_acquire))) ready->wait(nullptr, std::memory_order_acquire);
        t_current_xthread = this;
        xe::threading::gow2_SetCurrentThread(handle);
        const int result = ExecuteHost();
        xe::threading::gow2_SetCurrentThread(nullptr);
        t_current_xthread = nullptr;
        return uint32_t(result);
      },
      stack_size_, thread_name_.empty() ? std::string("XHostThread") : thread_name_);
  if (!guest_) {
    // The CommandProcessor ignores the result: it would hang without the thread.
    xe::FatalError("cannot create guest thread '" + thread_name_ + "'");
  }
  thread_ = std::make_unique<Handle>(guest_);
  thread_->set_name(thread_name_);
  ready->store(thread_.get(), std::memory_order_release);
  ready->notify_all();
  return kStatusSuccess;
}

uint32_t XThread::Wait(uint32_t, uint32_t, uint32_t, uint64_t*) {
  if (guest_) WaitGuestThread(guest_, nullptr);
  return kStatusSuccess;
}

}  // namespace kernel
}  // namespace xe
