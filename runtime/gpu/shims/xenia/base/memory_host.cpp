// Host functions of Xenia's src/xenia/base/memory.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec and
// xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad, BSD-3-Clause), which
// Xenia defines in memory_win.cc with the Windows APIs. Only those used by the
// vendored GPU code are here: guest memory is managed by
// runtime/kernel, not Xenia. AllocFixed/DeallocFixed (canary: SharedMemory's
// page flags) as in memory_win.cc, with VirtualAlloc.
#include "xenia/base/memory.h"

#include <windows.h>

namespace xe {
namespace memory {

// Windows x64 host page.
size_t page_size() { return 4096; }

// VirtualAlloc granularity on Windows x64.
size_t allocation_granularity() { return 64 * 1024; }

// No code generated at runtime in the process (no Xenia JIT).
bool IsWritableExecutableMemorySupported() { return false; }

static DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      return PAGE_NOACCESS;
  }
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type, PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
  }
  return VirtualAlloc(base_address, length, alloc_type, ToWin32ProtectFlags(access));
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  DWORD free_type = MEM_DECOMMIT;
  if (deallocation_type == DeallocationType::kRelease) {
    free_type = MEM_RELEASE;
    length = 0;
  }
  return VirtualFree(base_address, length, free_type) != FALSE;
}

}  // namespace memory
}  // namespace xe
