// Shim of Xenia's src/xenia/kernel/xthread.h (and of object_ref from
// src/xenia/kernel/xobject.h)
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): the part used by the CommandProcessor (XHostThread, Create,
// Wait, set_name, thread(), IsInThread).
//
// XHostThread is a guest thread with a host body in gow2's kernel
// (kernel::GuestThread::CreateHost, runtime/kernel/threads.h): it has a PCR,
// KTHREAD and guest stack, so the GPU interrupts called from it
// (gpu::DispatchInterrupt) run on a guest thread as in Xenia.
// The implementation (xthread.cpp) builds with the runtime; this header stays
// includable from Xenia's code (C++17, no kernel headers).
#ifndef XENIA_KERNEL_XTHREAD_H_
#define XENIA_KERNEL_XTHREAD_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "xenia/base/threading.h"

namespace kernel {
class GuestThread;  // runtime/kernel/threads.h
}  // namespace kernel

namespace xe {
namespace kernel {

class KernelState;

// Xenia object_ref (xobject.h): reference to a kernel object. Here a
// shared_ptr; the constructor from a raw pointer takes ownership as in
// Xenia (newly created object, one reference).
template <typename T>
class object_ref {
 public:
  object_ref() = default;
  explicit object_ref(T* value) : value_(value) {}
  T* get() const { return value_.get(); }
  T* operator->() const { return value_.get(); }
  T& operator*() const { return *value_; }
  explicit operator bool() const { return value_ != nullptr; }
  void reset() { value_.reset(); }

 private:
  std::shared_ptr<T> value_;
};

class XThread {
 public:
  virtual ~XThread();

  // The current thread is `other` (Xenia: current thread's TLS).
  static bool IsInThread(XThread* other);
  // The current thread is an XThread.
  static bool IsInThread();

  const std::string& name() const { return thread_name_; }
  void set_name(const std::string_view name) { thread_name_ = std::string(name); }

  // Starts the guest thread; 0 (X_STATUS_SUCCESS). Without memory for the
  // thread: xe::FatalError (exit 5), because Xenia's CommandProcessor
  // ignores the result and would hang without the thread.
  uint32_t Create();
  // Waits for the thread to finish (Xenia's arguments are ignored: infinite
  // wait, not alertable). 0 = X_STATUS_SUCCESS.
  uint32_t Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                uint64_t* opt_timeout);

  // Threading handle of the guest thread (Suspend/Resume/wait); nullptr
  // before Create.
  xe::threading::Thread* thread() { return thread_.get(); }

 protected:
  XThread(KernelState* kernel_state, uint32_t stack_size);
  virtual int ExecuteHost() = 0;

 private:
  class Handle;
  uint32_t stack_size_;
  std::string thread_name_;
  std::shared_ptr<::kernel::GuestThread> guest_;
  std::unique_ptr<xe::threading::Thread> thread_;
};

class XHostThread : public XThread {
 public:
  // creation_flags and guest_process (canary) are ignored: Xenia passes them to
  // the host thread and the KTHREAD, neither used by the GPU.
  XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
              std::function<int()> host_fn, uint32_t /*guest_process*/ = 0)
      : XThread(kernel_state, stack_size), host_fn_(std::move(host_fn)) {}

 protected:
  int ExecuteHost() override { return host_fn_(); }

 private:
  std::function<int()> host_fn_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTHREAD_H_
