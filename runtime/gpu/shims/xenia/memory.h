// Shim of Xenia's src/xenia/memory.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): of xe::Memory only the physical part used by the GPU remains,
// plus TranslateVirtual (base + guest address, with the +0x1000 offset of the
// 0xE view: gow2::HostOffset) used by the APU (XmaContext).
// Guest memory is gow2's arena (runtime/guest_memory, runtime/kernel):
// physical memory is the shared section mapped at 0xA0000000 (and at the
// mirrors 0xC0000000/0xE0000000), so physical_membase = base + 0xA0000000
// and a physical address is translated with & 0x1FFFFFFF as in Xenia.
//
// Invalidation callbacks: trampolines to runtime/physical_watch, which
// watches the pages and calls the callbacks under the global lock
// (xe::global_critical_region = physwatch::GlobalLock()).
// EnablePhysicalMemoryAccessCallbacks -> physwatch::Watch; data providers are
// not supported (enable_data_providers=true is logged once and
// ignored). The destructor removes any callbacks still registered.
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <cstdint>
#include <utility>
#include <vector>

// Relative path: the Xenia targets (xenia_apu, xenia_gpu*) do not have runtime/
// in their include paths and must not see its other headers.
#include "../../../guest_address.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"  // like xenia-canary's memory.h (global_unique_lock_type)

namespace xe {

// From xenia-canary's xenia/memory.h: flags and state of guest pages.
enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

struct HeapAllocationInfo {
  uint32_t base_address;
  uint32_t allocation_base;
  uint32_t allocation_protect;
  uint32_t allocation_size;
  uint32_t region_size;
  uint32_t state;
  uint32_t protect;
};

// Canary's physical heap reduced to the two queries the GPU code makes. In
// gow2 physical memory (512 MB) is a mapped section, writable in its
// entirety: everything is accessible, as in Xenia @95a5c3e which did not do
// these checks.
class VirtualHeap {
 public:
  memory::PageAccess QueryRangeAccess(uint32_t /*low_address*/, uint32_t /*high_address*/) const {
    return memory::PageAccess::kReadWrite;
  }
  bool QueryRegionInfo(uint32_t /*base_address*/, HeapAllocationInfo* out_info) const {
    *out_info = {0, 0, kMemoryProtectRead | kMemoryProtectWrite, 0x20000000, 0x20000000,
                 kMemoryAllocationCommit, kMemoryProtectRead | kMemoryProtectWrite};
    return true;
  }
};

class Memory {
 public:
  static constexpr uint32_t kPhysicalBase = 0xA0000000;
  static constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;

  explicit Memory(uint8_t* guest_base);
  ~Memory();
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;

  uint8_t* virtual_membase() const { return virtual_membase_; }
  uint8_t* physical_membase() const { return physical_membase_; }

  // Guest virtual address -> host: arena base + address (in gow2 the whole
  // 32-bit guest space is reserved, as in Xenia). For virtual addresses
  // only: a physical address (e.g. the buffer pointers in the XMA contexts,
  // ContextArrayAddress) must be translated with TranslatePhysical, which
  // applies the 0x1FFFFFFF mask and the 0xA0000000 base; here it would end up
  // in the first 512 MiB of virtual space, i.e. in different memory. With the
  // 0xE0000000 view offset (runtime/guest_address.h), like the rest of gow2.
  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + gow2::HostOffset(guest_address));
  }

  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & kPhysicalMask));
  }

  // Returns the range (start, length) the caller can stop watching;
  // Xenia's signature.
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);
  // Handle for Unregister.
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

  // For tests: registered callbacks.
  size_t gow2_invalidation_callback_count() const;

  // xenia-canary: physical heap, for access checks before uploads
  // (D3D12SharedMemory::UploadRanges) and resolve readbacks
  // (D3D12CommandProcessor). See VirtualHeap.
  VirtualHeap* GetPhysicalHeap() { return &physical_heap_; }

 private:
  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
  // physwatch handles registered by this instance (under the global lock).
  std::vector<void*> invalidation_callbacks_;
  VirtualHeap physical_heap_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
