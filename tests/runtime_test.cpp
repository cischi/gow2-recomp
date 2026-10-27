// Tests of gow2_runtime: platform, log, guest arena and XEX loader.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "guest_address.h"
#include "guest_memory.h"
#include "image_loader.h"
#include "imports.h"
#include "log.h"
#include "platform/platform.h"
#include "xex_check.h"
#include "temp_dir.h"

// only for SEH in the guard test (the runtime uses the OS only from platform/)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef LoadImage  // windows.h macro: here the runtime's LoadImage is needed

namespace fs = std::filesystem;

namespace {

// Unique temporary folder with a space and [ ] in the name, removed at the end of the test.
struct TempDir : gow2_test::TempDir {
  explicit TempDir(const char* tag) : gow2_test::TempDir(std::string("gow2 runtime [") + tag + "]") {}
};

void WriteFile(const fs::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

TEST_CASE("runtime: 4 GB arena writable at the extremes after the commit") {
  GuestMemory m;
  REQUIRE(m.Init());
  REQUIRE(m.base != nullptr);
  // Init only reserves: the first and the last valid page are committed
  // (the last guest page, 0xFFFFF000, falls beyond the section because of the
  // 0xE view shift: its commit fails).
  REQUIRE(m.Commit(0, 1));
  REQUIRE(m.Commit(0xFFFFEFFFu, 1));
  CHECK_FALSE(m.Commit(0xFFFFF000u, 1));
  CHECK_FALSE(m.Decommit(0xFFFFFFFFu, 1));
  // the last valid guest byte is the last host byte of the 4 GB (physical 0x1FFFFFFF)
  REQUIRE(gow2::HostOffset(0xFFFFEFFFu) == GuestMemory::kSize - 1);
  CHECK(m.base[0] == 0);
  CHECK(m.base[GuestMemory::kSize - 1] == 0);
  m.base[0] = 1;
  m.base[GuestMemory::kSize - 1] = 1;
  CHECK(m.base[0] == 1);
  CHECK(m.base[GuestMemory::kSize - 1] == 1);
  CHECK(m.base[0xBFFFFFFFu] == 1);  // same physical byte from the 0xA view
}

TEST_CASE("runtime: 0xE view shifted by 0x1000 in the physical memory") {
  GuestMemory m;
  REQUIRE(m.Init());
  constexpr uint32_t x = 0x5000;
  // a byte written from 0xE0000000 + x is physical x + 0x1000 (0xA view)
  m.base[gow2::HostOffset(0xE0000000u + x)] = 0x5A;
  CHECK(m.base[0xA0000000u + x + 0x1000] == 0x5A);
  CHECK(m.base[0xC0000000u + x + 0x1000] == 0x5A);
  CHECK(m.base[0xA0000000u + x] == 0);

  // Commit(0xE0004000) zeroes physical 0x5000, not physical 0x4000
  std::memset(m.base + 0xA0004000u, 0x11, 0x1000);
  std::memset(m.base + 0xA0005000u, 0x22, 0x1000);
  REQUIRE(m.Commit(0xE0004000u, 0x1000));
  CHECK(m.base[0xA0004000u] == 0x11);
  CHECK(m.base[0xA0004FFFu] == 0x11);
  CHECK(m.base[0xA0005000u] == 0);
  CHECK(m.base[0xA0005FFFu] == 0);
  CHECK(m.base[0xA0006000u] == 0x5A);  // the next page stays intact

  // Symmetric Decommit: the pages stay committed, readable and intact
  std::memset(m.base + 0xA0005000u, 0x33, 0x1000);
  REQUIRE(m.Decommit(0xE0004000u, 0x1000));
  CHECK(m.base[gow2::HostOffset(0xE0004000u)] == 0x33);
  CHECK(m.base[0xA0005FFFu] == 0x33);
  CHECK(m.base[0xA0004000u] == 0x11);

  // a range that reaches the last page fails
  CHECK_FALSE(m.Commit(0xFFFFE000u, 0x2000));
  CHECK_FALSE(m.Decommit(0xFFFFE000u, 0x2000));
}

TEST_CASE("runtime: repeated Init and destruction also release the tail reserve") {
  uint8_t* first = nullptr;
  for (int i = 0; i < 3; ++i) {
    GuestMemory m;
    REQUIRE(m.Init());
    if (!first) first = m.base;
    // if the tail stayed reserved the placeholder would not come back to the same place
    CHECK(m.base == first);
  }
}

TEST_CASE("runtime: Alloc aligned and increasing") {
  GuestMemory m;
  REQUIRE(m.Init());
  const uint32_t a = m.Alloc(16, 0x1000);
  const uint32_t b = m.Alloc(16, 0x1000);
  REQUIRE(a != 0);
  REQUIRE(a % 0x1000 == 0);
  REQUIRE(b % 0x1000 == 0);
  REQUIRE(b > a);
  REQUIRE(b - a >= 16);
}

TEST_CASE("runtime: Alloc starts from SetHeapStart and fails with 0") {
  GuestMemory m;
  REQUIRE(m.Init());
  m.SetHeapStart(0x90000010);
  CHECK(m.Alloc(4, 0x100) == 0x90000100);
  CHECK(m.Alloc(4, 0) == 0x90000104);    // align 0 = 1
  CHECK(m.Alloc(4, 3) == 0);             // not a power of 2
  CHECK(m.Alloc(0x80000000u, 1) == 0);   // beyond 4 GB
  CHECK(m.Alloc(4, 4) == 0x90000108);    // a failure does not move the heap
}

TEST_CASE("runtime: Alloc does not enter the physical heap (0xA0000000)") {
  GuestMemory m;
  REQUIRE(m.Init());
  m.SetHeapStart(0x9FFF0000);
  CHECK(m.Alloc(0x10000, 0x1000) == 0x9FFF0000);  // up to 0xA0000000 excluded
  CHECK(m.Alloc(1, 1) == 0);
  m.SetHeapStart(0x9FFFF000);
  CHECK(m.Alloc(0x2000, 0x1000) == 0);  // would exceed the limit
}

TEST_CASE("runtime: ReadWholeFile") {
  TempDir dir("read");
  CHECK_FALSE(plat::ReadWholeFile(dir.path / "missing.bin").has_value());
  WriteFile(dir.path / "data.bin", std::string("ab\0c", 4));
  auto data = plat::ReadWholeFile(dir.path / "data.bin");
  REQUIRE(data.has_value());
  CHECK(*data == std::vector<uint8_t>{'a', 'b', 0, 'c'});
}

TEST_CASE("runtime: LogLine writes to the log file") {
  TempDir dir("log");
  const fs::path log = dir.path / "gow2.log";
  LogInit(log);
  LogLine("first line");
  LogLine("second line");
  // immediate flush: the content is already on disk without closing the log
  std::ifstream in(log, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content == "first line\nsecond line\n");
  in.close();
  LogInit({});  // closes the file before the cleanup
}

TEST_CASE("runtime: LogLine from several threads writes whole lines") {
  TempDir dir("log-mt");
  const fs::path log = dir.path / "gow2.log";
  LogInit(log);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([t] {
      for (int m = 0; m < 200; ++m)
        LogLine("[t" + std::to_string(t) + "] line " + std::to_string(m) + " " + std::string(40, 'x'));
    });
  for (auto& th : threads) th.join();
  LogInit({});
  std::ifstream in(log, std::ios::binary);
  const std::regex re(R"(^\[t\d\] line \d+ x{40}$)");
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    CHECK(std::regex_match(line, re));
    ++n;
  }
  CHECK(n == 1600);
}

TEST_CASE("runtime: missing xex -> FileMissing") {
  GuestMemory m;
  REQUIRE(m.Init());
  auto r = LoadImage(m, "C:/does not exist [x]/default.xex");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == LoadError::FileMissing);
}

TEST_CASE("runtime: non-xex file -> ParseFailed") {
  GuestMemory m;
  REQUIRE(m.Init());
  TempDir dir("garbage");
  WriteFile(dir.path / "garbage.xex", "garbage");
  auto r = LoadImage(m, dir.path / "garbage.xex");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == LoadError::ParseFailed);

  // XEX2 magic but inconsistent headers: no crash inside ParseImage
  WriteFile(dir.path / "fake.xex", std::string("XEX2") + std::string(60, '\xff'));
  auto r2 = LoadImage(m, dir.path / "fake.xex");
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.error() == LoadError::ParseFailed);
}

TEST_CASE("runtime: real xex loaded", "[real]") {
  const fs::path xex = GOW2_DEFAULT_XEX;
  if (!fs::exists(xex)) SKIP("missing " << xex.string());

  GuestMemory m;
  REQUIRE(m.Init());
  auto r = LoadImage(m, xex);
  REQUIRE(r.has_value());
  CHECK(r->entry == uint32_t(GOW2_EXPECTED_ENTRY));
  REQUIRE(r->entry >= r->base);
  REQUIRE(r->entry < uint64_t(r->base) + r->size);
  CHECK(m.base[r->base] == 'M');
  CHECK(m.base[r->base + 1] == 'Z');
  CHECK(Be32(m.base + r->entry) != 0);

  // tail not produced by XenonUtils' loader: zero (not read beyond the buffer)
  auto file = plat::ReadWholeFile(xex);
  REQUIRE(file.has_value());
  auto produced = xexcheck::ValidateXex(*file);
  REQUIRE(produced.has_value());
  for (uint64_t off = *produced; off < r->size; ++off) {
    if (m.base[r->base + off] != 0) FAIL("non-zero byte in the tail at +0x" << std::hex << off);
  }

  // import records read from header 0x000103FF with the original values
  CHECK(r->imports.size() == size_t(GOW2_EXPECTED_IMPORT_COUNT));
  size_t functions = 0;
  for (const auto& im : r->imports) {
    REQUIRE((im.type == 0 || im.type == 1));
    CHECK((im.module == "xboxkrnl.exe" || im.module == "xam.xex"));
    CHECK(im.thunk >= r->base);
    CHECK(im.thunk < uint64_t(r->base) + r->size);
    if (im.type == 1) ++functions;
    // GoW2: all ordinals have a name in XenonUtils' tables
    CHECK(ImportName(im).rfind("ordinal_", 0) == std::string::npos);
  }
  CHECK(functions > 0);
  // ParseImage did not rewrite the records: a function's thunk still
  // contains the original descriptor (type 1 in the high byte)
  for (const auto& im : r->imports)
    if (im.type == 1) CHECK(Be32(m.base + im.thunk) >> 24 == 1);

  // the heap starts after the image, rounded to 64 KB
  const uint64_t heap = (uint64_t(r->base) + r->size + 0xFFFF) & ~uint64_t(0xFFFF);
  CHECK(m.HeapNext() == heap);
  const uint32_t a = m.Alloc(16, 16);
  CHECK(a == heap);
}

TEST_CASE("runtime: ResolveImports links function and variable slots") {
  GuestMemory m;
  REQUIRE(m.Init());
  m.SetHeapStart(0x90000000);
  const std::vector<ImportRecord> records = {
      {"xboxkrnl.exe", 0x82000000, 0x1F5, 0},  // slot of an imported function
      {"xboxkrnl.exe", 0x82000004, 0x1B, 0},   // kernel variable
      {"xam.xex", 0x82000008, 0x1B, 0},        // same ordinal, other module: variable
      {"xboxkrnl.exe", 0x82000010, 0x1F5, 1},  // the function's thunk
  };
  REQUIRE(m.Commit(0x82000000, 0x14));  // slots of the records (in the game: the image)
  auto stats = ResolveImports(m, records);
  REQUIRE(stats.has_value());
  CHECK(stats->function_thunks == 1);
  CHECK(stats->function_slots == 1);
  CHECK(stats->variables == 2);

  CHECK(Be32(m.base + 0x82000000) == 0x82000010);
  const uint32_t v1 = Be32(m.base + 0x82000004);
  const uint32_t v2 = Be32(m.base + 0x82000008);
  CHECK(v1 >= 0x90000000);
  CHECK(v2 >= 0x90000000);
  CHECK(v1 != v2);
  for (uint32_t i = 0; i < kVariableImportSize; ++i) {
    if (m.base[v1 + i] != 0 || m.base[v2 + i] != 0) FAIL("variable not zeroed at +" << i);
  }
  CHECK(m.HeapNext() >= v2 + kVariableImportSize);
}

TEST_CASE("runtime: ResolveImports rejects invalid records") {
  GuestMemory m;
  REQUIRE(m.Init());
  const std::vector<ImportRecord> bad_type = {{"xboxkrnl.exe", 0x82000000, 1, 2}};
  CHECK_FALSE(ResolveImports(m, bad_type).has_value());
  const std::vector<ImportRecord> out_of_range = {{"xboxkrnl.exe", 0xFFFFFFFE, 1, 0}};
  CHECK_FALSE(ResolveImports(m, out_of_range).has_value());
}

TEST_CASE("runtime: ImportName uses XenonUtils' tables") {
  CHECK(ImportName({"xboxkrnl.exe", 0, 0x1F5, 1}) == "XAudioSubmitRenderDriverFrame");
  CHECK(ImportName({"foo.dll", 0, 0x1F5, 1}) == "ordinal_0x01F5");
}

TEST_CASE("runtime: FormatImportCall lists r3..r10") {
  const uint64_t regs[8] = {0x1, 0, 0x82000000, 0xFFFFFFFFFFFFFFFFull, 5, 6, 7, 0xABCDEF};
  CHECK(FormatImportCall("KeTlsAlloc", regs) ==
        "[kernel] KeTlsAlloc(r3=0x1, r4=0x0, r5=0x82000000, r6=0xFFFFFFFFFFFFFFFF, r7=0x5, r8=0x6, "
        "r9=0x7, r10=0xABCDEF)");
}

namespace {
// Catch2 registers a vectored exception handler that reports every access
// violation as a fatal error before any __except: the test registers
// one with higher priority that, for a fault in the region under
// observation, records the address, makes the page writable and has the
// instruction retried.
uintptr_t g_watch_begin = 0, g_watch_end = 0;
volatile uintptr_t g_fault = 0;

LONG CALLBACK RecordFault(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t addr = uintptr_t(rec->ExceptionInformation[1]);
  if (addr < g_watch_begin || addr >= g_watch_end) return EXCEPTION_CONTINUE_SEARCH;
  g_fault = addr;
  DWORD old = 0;
  void* page = reinterpret_cast<void*>(addr & ~uintptr_t(0xFFF));
  // page that is only reserved (GuestMemory's tail reserve): it is committed
  if (!VirtualProtect(page, 0x1000, PAGE_READWRITE, &old) &&
      !VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE))
    return EXCEPTION_CONTINUE_SEARCH;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Address of the access violation caused by the write, 0 if none.
uintptr_t FaultingWrite(volatile uint8_t* p) {
  g_fault = 0;
  *p = 1;
  return g_fault;
}
}  // namespace

TEST_CASE("runtime: ProtectNoAccess makes writes fail only in the guard") {
  constexpr uint64_t kPage = 0x1000;
  auto* p = static_cast<uint8_t*>(plat::ReserveCommit(0, 3 * kPage));
  REQUIRE(p != nullptr);
  g_watch_begin = uintptr_t(p);
  g_watch_end = uintptr_t(p + 3 * kPage);
  void* handler = AddVectoredExceptionHandler(1, &RecordFault);
  REQUIRE(handler != nullptr);

  REQUIRE(plat::ProtectNoAccess(p + kPage, kPage));
  CHECK(FaultingWrite(p) == 0);
  CHECK(FaultingWrite(p + kPage) == uintptr_t(p + kPage));
  REQUIRE(plat::ProtectNoAccess(p + kPage, kPage));  // the handler had reopened it
  CHECK(FaultingWrite(p + 2 * kPage - 1) == uintptr_t(p + 2 * kPage - 1));
  CHECK(FaultingWrite(p + 2 * kPage) == 0);
  CHECK_FALSE(plat::ProtectNoAccess(nullptr, kPage));

  RemoveVectoredExceptionHandler(handler);
  plat::Release(p, 3 * kPage);
}

TEST_CASE("runtime: the last guest page falls in the tail reserve and faults") {
  GuestMemory m;
  REQUIRE(m.Init());
  uint8_t* tail = m.base + GuestMemory::kSize;
  // the tail is a GuestMemory reserve, not committed, kTailReserve long
  MEMORY_BASIC_INFORMATION mbi{};
  REQUIRE(VirtualQuery(tail, &mbi, sizeof(mbi)) == sizeof(mbi));
  REQUIRE(mbi.State == MEM_RESERVE);
  REQUIRE(mbi.AllocationBase == tail);
  CHECK(mbi.RegionSize == GuestMemory::kTailReserve);
  STATIC_REQUIRE(GuestMemory::kTailReserve >= gow2::kViewEOffset);

  // the last guest byte lies inside the tail
  uint8_t* last_page = m.base + gow2::HostOffset(0xFFFFF000u);
  uint8_t* last_byte = m.base + gow2::HostOffset(0xFFFFFFFFu);
  CHECK(last_page == tail);
  CHECK(last_byte < tail + GuestMemory::kTailReserve);

  g_watch_begin = uintptr_t(tail);
  g_watch_end = uintptr_t(tail + GuestMemory::kTailReserve);
  void* handler = AddVectoredExceptionHandler(1, &RecordFault);
  REQUIRE(handler != nullptr);
  CHECK(FaultingWrite(last_page) == uintptr_t(last_page));
  CHECK(FaultingWrite(last_byte) == 0);  // same page, committed by the test's handler
  RemoveVectoredExceptionHandler(handler);
}
