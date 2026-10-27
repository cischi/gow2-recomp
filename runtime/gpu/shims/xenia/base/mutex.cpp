// Definition of xenia-canary's src/xenia/base/mutex.cc
// (https://github.com/xenia-canary/xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause) for the vendored header xenia/base/mutex.h: Xenia's recursive
// global lock is the one for physical page watching
// (runtime/physical_watch.h, which documents the lock order), as in Xenia
// where global_critical_region also protects the invalidation callbacks.
//
// In canary global_mutex_type is xe_global_mutex (recursive SRWLOCK): here its
// methods forward to physwatch::GlobalLock(), so there is a single lock for
// Xenia and for the runtime (the object's SRWLOCK fields are unused).
// xe_fast_mutex is canary's.
#include "xenia/base/mutex.h"

#include "physical_watch.h"
#include "xenia/base/assert.h"

namespace xe {

void xe_global_mutex::lock() { physwatch::GlobalLock().lock(); }
void xe_global_mutex::unlock() { physwatch::GlobalLock().unlock(); }
bool xe_global_mutex::try_lock() { return physwatch::GlobalLock().try_lock(); }

global_mutex_type& global_critical_region::mutex() {
  static global_mutex_type global_mutex;
  return global_mutex;
}

// xe_fast_mutex: non-recursive mutex via SRWLOCK
void xe_fast_mutex::lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    assert_always("xe_fast_mutex: recursive lock detected");
  }
  AcquireSRWLockExclusive(&srwlock_);
  owner_thread_ = self;
}

void xe_fast_mutex::unlock() {
  owner_thread_ = 0;
  ReleaseSRWLockExclusive(&srwlock_);
}

bool xe_fast_mutex::try_lock() {
  if (TryAcquireSRWLockExclusive(&srwlock_)) {
    owner_thread_ = GetCurrentThreadId();
    return true;
  }
  return false;
}

}  // namespace xe
