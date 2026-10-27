// Shim of Xenia's src/xenia/memory.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): Memory::RegisterPhysicalMemoryInvalidationCallback /
// UnregisterPhysicalMemoryInvalidationCallback / EnablePhysicalMemoryAccessCallbacks
// forwarded to runtime/physical_watch (page watching, callbacks and
// global lock). Register, Unregister and the callback invocation are
// serialized by the global lock: after Unregister the callback is not
// running and will not be called again.
#include "xenia/memory.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "log.h"
#include "physical_watch.h"

namespace xe {

Memory::Memory(uint8_t* guest_base)
    : virtual_membase_(guest_base), physical_membase_(guest_base + kPhysicalBase) {}

Memory::~Memory() {
  std::lock_guard lock(physwatch::GlobalLock());
  for (void* handle : invalidation_callbacks_) {
    physwatch::Unregister(handle);
  }
  invalidation_callbacks_.clear();
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                         void* callback_context) {
  std::lock_guard lock(physwatch::GlobalLock());
  void* handle = physwatch::Register(callback, callback_context);
  invalidation_callbacks_.push_back(handle);
  return handle;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
  std::lock_guard lock(physwatch::GlobalLock());
  auto it = std::find(invalidation_callbacks_.begin(), invalidation_callbacks_.end(), callback_handle);
  if (it == invalidation_callbacks_.end()) {
    return;
  }
  invalidation_callbacks_.erase(it);
  physwatch::Unregister(callback_handle);
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications,
                                                 bool enable_data_providers) {
  if (enable_data_providers) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      LogLine("[gpu] EnablePhysicalMemoryAccessCallbacks: data providers not supported, ignored");
    }
  }
  if (enable_invalidation_notifications) {
    physwatch::Watch(physical_address & kPhysicalMask, length);
  }
}

size_t Memory::gow2_invalidation_callback_count() const {
  std::lock_guard lock(physwatch::GlobalLock());
  return invalidation_callbacks_.size();
}

}  // namespace xe
