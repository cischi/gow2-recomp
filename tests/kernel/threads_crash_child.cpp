// Child process of threads_test ("stack guard -> stack overflow"):
// creates a guest thread that writes below its stack, into the guard, with
// gow2.exe's crash handler (FormatCrashLine + LogLineFromCrash, exit 3).
// No Catch2: its exception handler would intercept the crash.
//
//   kernel_threads_crash_child <log file>
// Exit 3 from the crash handler; 0 if the write does not fail (error); 1
// if the environment does not set up.
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "crash_map.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"
#include "platform/platform.h"

namespace {

constexpr uint32_t kWriteGuard = 0x82000000;

// Writes 16 bytes below the bottom of the stack (PCR + 0x74), inside the guard.
PPC_FUNC(WriteGuard) {
  const uint32_t pcr = ctx.r13.u32;
  const uint32_t stack_end = *reinterpret_cast<be<uint32_t>*>(base + pcr + kernel::kPcrStackEnd);
  *reinterpret_cast<volatile uint32_t*>(base + stack_end - 16) = 0xDEADBEEF;
  ctx.r3.u64 = 0;
}

PPCFunc* Resolve(uint32_t guest) { return guest == kWriteGuard ? &WriteGuard : nullptr; }

void OnCrash(const plat::CrashInfo& info) {
  char line[256];
  FormatCrashLine(line, sizeof(line), info);
  LogLineFromCrash(line);
  std::_Exit(3);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  LogInit(argv[1]);
  static GuestMemory mem;
  if (!mem.Init()) return 1;
  kernel::g_base = mem.base;
  if (!kernel::InitMemory(mem) || !kernel::InitThreads(mem, &Resolve, 0)) return 1;
  plat::InstallCrashHandler(&OnCrash);
  auto t = kernel::GuestThread::Create(kWriteGuard, 0, 0x10000, false);
  if (!t) return 1;
  const std::shared_ptr<kernel::KernelObject> one[1] = {t};
  const int64_t timeout = -int64_t(10) * 1000 * 10000;  // 10 s
  kernel::WaitAny(one, &timeout, false);
  LogLine("no crash");
  std::_Exit(0);
}
