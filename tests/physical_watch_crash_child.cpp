// Child process of physical_watch_test: with physwatch's write handler and
// gow2.exe's crash handler (FormatCrashLine +
// LogLineFromCrash, exit 3), a write to a watched page is handled,
// while a write to a physical page that is not watched but protected
// (ProtectNoAccess) reaches the crash handler: the VEH does not steal the AV.
// No Catch2: its exception filter would intercept the crash.
//
//   physical_watch_crash_child <log file>
// Exit 3 from the crash handler; 0 if the write does not fail (error); 1
// if the environment cannot be set up or the watched page misbehaves.
#include <cstdint>
#include <cstdlib>

#include "crash_map.h"
#include "guest_memory.h"
#include "log.h"
#include "physical_watch.h"
#include "platform/platform.h"

namespace {

int g_calls = 0;

std::pair<uint32_t, uint32_t> OnInvalidate(void*, uint32_t phys, uint32_t len, bool) {
  ++g_calls;
  return {phys, len};
}

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
  physwatch::Init(mem);  // as in main.cpp: before the crash handler
  plat::InstallCrashHandler(&OnCrash);
  physwatch::Register(&OnInvalidate, nullptr);

  // watched page: the write is handled
  physwatch::Watch(0x100000, 0x1000);
  *reinterpret_cast<volatile uint32_t*>(mem.base + 0xC0100010) = 0x12345678;
  if (g_calls != 1 || *reinterpret_cast<volatile uint32_t*>(mem.base + 0xA0100010) != 0x12345678) return 1;
  LogLine("watched ok");

  // physical page protected but not watched: a real crash
  if (!plat::ProtectNoAccess(mem.base + 0xA0200000, 0x1000)) return 1;
  *reinterpret_cast<volatile uint32_t*>(mem.base + 0xA0200000) = 0xDEADBEEF;
  LogLine("no crash");
  std::_Exit(0);
}
