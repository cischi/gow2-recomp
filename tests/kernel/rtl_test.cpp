// Rtl/Ke/Hal utilities of runtime/kernel/rtl_exports.cpp on synthetic guest
// structures in a scratch arena: no game bytes.
//
// KeBugCheck and HalReturnToFirmware terminate the process: they are tried in a
// child process. Choice: the child is the same test executable, relaunched
// with only the hidden test "[.rtl_child]" and the environment variables
// GOW2_RTL_TEST_CHILD (= "bugcheck" | "bugcheckex" | "halreturn") and
// GOW2_RTL_TEST_LOG (log path). That test calls the export and never
// returns: no helper executable in CMake nor a custom main is needed.
// (A static initializer would not do: it would run before log.cpp's globals
// are constructed.) Without the variables the hidden test does nothing.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "kernel/guest.h"
#include "kernel/rtl_exports.h"
#include "log.h"
#include "platform/platform.h"
#include "../temp_dir.h"

PPC_EXTERN_FUNC(__imp__RtlInitAnsiString);
PPC_EXTERN_FUNC(__imp__RtlInitUnicodeString);
PPC_EXTERN_FUNC(__imp__RtlFreeAnsiString);
PPC_EXTERN_FUNC(__imp__RtlUnicodeStringToAnsiString);
PPC_EXTERN_FUNC(__imp__RtlCompareString);
PPC_EXTERN_FUNC(__imp__RtlCompareMemory);
PPC_EXTERN_FUNC(__imp__RtlCompareMemoryUlong);
PPC_EXTERN_FUNC(__imp__RtlFillMemoryUlong);
PPC_EXTERN_FUNC(__imp__RtlNtStatusToDosError);
PPC_EXTERN_FUNC(__imp__RtlTimeToTimeFields);
PPC_EXTERN_FUNC(__imp__RtlTimeFieldsToTime);
PPC_EXTERN_FUNC(__imp__KeQuerySystemTime);
PPC_EXTERN_FUNC(__imp__KeQueryPerformanceFrequency);
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__XexCheckExecutablePrivilege);
PPC_EXTERN_FUNC(__imp__KeBugCheck);
PPC_EXTERN_FUNC(__imp__KeBugCheckEx);
PPC_EXTERN_FUNC(__imp__HalReturnToFirmware);
PPC_EXTERN_FUNC(__imp__DbgPrint);
PPC_EXTERN_FUNC(__imp__RtlMultiByteToUnicodeN);
PPC_EXTERN_FUNC(__imp__RtlUnicodeToMultiByteN);
PPC_EXTERN_FUNC(__imp__ExGetXConfigSetting);
PPC_EXTERN_FUNC(__imp__ExRegisterTitleTerminateNotification);

namespace {

constexpr size_t kArenaSize = 0x10000;
uint8_t* Arena() {
  alignas(64) static uint8_t arena[kArenaSize];
  return arena;
}

void PutBe16(uint32_t a, uint16_t v) { *reinterpret_cast<be<uint16_t>*>(Arena() + a) = v; }
void PutBe32(uint32_t a, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(Arena() + a) = v; }
void PutBe64(uint32_t a, uint64_t v) { *reinterpret_cast<be<uint64_t>*>(Arena() + a) = v; }
uint16_t GetBe16(uint32_t a) { return *reinterpret_cast<be<uint16_t>*>(Arena() + a); }
uint32_t GetBe32(uint32_t a) { return *reinterpret_cast<be<uint32_t>*>(Arena() + a); }
uint64_t GetBe64(uint32_t a) { return *reinterpret_cast<be<uint64_t>*>(Arena() + a); }

void PutCString(uint32_t a, const char* s) { std::memcpy(Arena() + a, s, std::strlen(s) + 1); }
void PutUtf16(uint32_t a, const char16_t* s) {
  for (; *s; ++s, a += 2) PutBe16(a, *s);
  PutBe16(a, 0);
}

// ANSI_STRING / UNICODE_STRING guest.
void PutString(uint32_t at, uint16_t len, uint16_t max, uint32_t buf) {
  PutBe16(at, len);
  PutBe16(at + 2, max);
  PutBe32(at + 4, buf);
}

// Calls an export with integer arguments in r3..; returns r3.
uint64_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args, uint64_t lr = 0) {
  kernel::g_base = Arena();
  PPCContext ctx{};
  ctx.r1.u64 = 0xF000;
  ctx.lr = lr;
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  fn(ctx, Arena());
  return ctx.r3.u64;
}

// ---- child process ------------------------------------------------------

struct ChildResult {
  int exit_code;
  std::string log;
};

ChildResult RunChild(const char* mode) {
  const auto log = gow2_test::UniqueTempPath(std::string("gow2_rtl_child_") + mode);
  _putenv_s("GOW2_RTL_TEST_CHILD", mode);
  _putenv_s("GOW2_RTL_TEST_LOG", log.string().c_str());
  const std::string cmd = "\"\"" + plat::ExecutablePath().string() + "\" \"[.rtl_child]\" >nul 2>&1\"";
  const int rc = std::system(cmd.c_str());
  _putenv_s("GOW2_RTL_TEST_CHILD", "");
  _putenv_s("GOW2_RTL_TEST_LOG", "");
  std::ifstream in(log);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(log);
  return {rc, ss.str()};
}

bool Contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

// ---- strings ------------------------------------------------------------

TEST_CASE("RtlInitAnsiString: string, empty and null") {
  PutCString(0x100, "hello");
  Call(__imp__RtlInitAnsiString, {0x200, 0x100});
  CHECK(GetBe16(0x200) == 5);
  CHECK(GetBe16(0x202) == 6);
  CHECK(GetBe32(0x204) == 0x100);

  PutCString(0x110, "");
  Call(__imp__RtlInitAnsiString, {0x200, 0x110});
  CHECK(GetBe16(0x200) == 0);
  CHECK(GetBe16(0x202) == 1);
  CHECK(GetBe32(0x204) == 0x110);

  Call(__imp__RtlInitAnsiString, {0x200, 0});
  CHECK(GetBe16(0x200) == 0);
  CHECK(GetBe16(0x202) == 0);
  CHECK(GetBe32(0x204) == 0);
}

TEST_CASE("RtlInitUnicodeString: big-endian UTF-16, empty and null") {
  PutUtf16(0x300, u"abc");
  Call(__imp__RtlInitUnicodeString, {0x400, 0x300});
  CHECK(GetBe16(0x400) == 6);
  CHECK(GetBe16(0x402) == 8);
  CHECK(GetBe32(0x404) == 0x300);

  PutUtf16(0x310, u"");
  Call(__imp__RtlInitUnicodeString, {0x400, 0x310});
  CHECK(GetBe16(0x400) == 0);
  CHECK(GetBe16(0x402) == 2);

  Call(__imp__RtlInitUnicodeString, {0x400, 0});
  CHECK(GetBe16(0x400) == 0);
  CHECK(GetBe16(0x402) == 0);
  CHECK(GetBe32(0x404) == 0);
}

namespace {
uint32_t g_next_alloc = 0x8000;
uint32_t g_freed = 0;
uint32_t TestAlloc(uint32_t size) {
  const uint32_t a = g_next_alloc;
  g_next_alloc += (size + 15) & ~15u;
  return a;
}
void TestFree(uint32_t addr) { g_freed = addr; }
}  // namespace

TEST_CASE("RtlUnicodeStringToAnsiString: ASCII, non-ASCII (UTF-8 like Xenia), small buffer, allocation") {
  PutUtf16(0x500, u"Ab");
  PutString(0x600, 4, 6, 0x500);  // UNICODE_STRING "Ab"
  PutString(0x610, 0, 8, 0x700);  // ANSI_STRING with an 8-byte buffer
  CHECK(Call(__imp__RtlUnicodeStringToAnsiString, {0x610, 0x600, 0}) == 0);
  CHECK(GetBe16(0x610) == 2);
  CHECK(std::string(reinterpret_cast<char*>(Arena() + 0x700)) == "Ab");

  // non-ASCII character: U+00E9 -> two UTF-8 bytes (xe::to_utf8), not '?'
  PutUtf16(0x520, u"aé");
  PutString(0x620, 4, 6, 0x520);
  PutString(0x630, 0, 8, 0x720);
  CHECK(Call(__imp__RtlUnicodeStringToAnsiString, {0x630, 0x620, 0}) == 0);
  CHECK(GetBe16(0x630) == 3);
  CHECK(std::memcmp(Arena() + 0x720, "a\xC3\xA9", 4) == 0);

  // buffer too small: STATUS_BUFFER_OVERFLOW, writes what fits, 0-terminated
  PutString(0x640, 0, 2, 0x740);
  CHECK(Call(__imp__RtlUnicodeStringToAnsiString, {0x640, 0x600, 0}) == 0x80000005);
  CHECK(std::string(reinterpret_cast<char*>(Arena() + 0x740)) == "A");

  // allocation: without a heap -> STATUS_NO_MEMORY
  kernel::rtl::SetSystemHeap(nullptr, nullptr);
  PutString(0x650, 0, 0, 0);
  CHECK(Call(__imp__RtlUnicodeStringToAnsiString, {0x650, 0x600, 1}) == 0xC0000017);

  // with a heap: buffer allocated, Length/MaximumLength/Buffer written; Free releases it and zeroes
  kernel::rtl::SetSystemHeap(&TestAlloc, &TestFree);
  CHECK(Call(__imp__RtlUnicodeStringToAnsiString, {0x650, 0x600, 1}) == 0);
  CHECK(GetBe16(0x650) == 2);
  CHECK(GetBe16(0x652) == 3);
  const uint32_t buf = GetBe32(0x654);
  CHECK(buf == 0x8000);
  CHECK(std::string(reinterpret_cast<char*>(Arena() + buf)) == "Ab");
  Call(__imp__RtlFreeAnsiString, {0x650});
  CHECK(g_freed == buf);
  CHECK(GetBe16(0x650) == 0);
  CHECK(GetBe16(0x652) == 0);
  CHECK(GetBe32(0x654) == 0);
  kernel::rtl::SetSystemHeap(nullptr, nullptr);
}

TEST_CASE("RtlCompareString: ANSI_STRING, upper/lower case, lengths") {
  PutCString(0x800, "abc");
  PutCString(0x810, "ABD");
  PutCString(0x820, "ab");
  PutString(0x900, 3, 4, 0x800);
  PutString(0x910, 3, 4, 0x810);
  PutString(0x920, 2, 3, 0x820);
  CHECK(int32_t(Call(__imp__RtlCompareString, {0x900, 0x900, 0})) == 0);
  CHECK(int32_t(Call(__imp__RtlCompareString, {0x900, 0x910, 0})) > 0);   // 'a' > 'A'
  CHECK(int32_t(Call(__imp__RtlCompareString, {0x900, 0x910, 1})) < 0);   // 'C' < 'D'
  CHECK(int32_t(Call(__imp__RtlCompareString, {0x900, 0x920, 0})) > 0);   // prefix: the longer one
  CHECK(int32_t(Call(__imp__RtlCompareString, {0x920, 0x900, 0})) < 0);
}

// ---- memory -------------------------------------------------------------

TEST_CASE("RtlCompareMemory counts equal bytes; Ulong and Fill respect big-endian and alignment") {
  PutCString(0xA00, "abcdef");
  PutCString(0xA20, "abXdeY");
  CHECK(Call(__imp__RtlCompareMemory, {0xA00, 0xA20, 6}) == 4);
  CHECK(Call(__imp__RtlCompareMemory, {0xA00, 0xA20, 0}) == 0);

  Call(__imp__RtlFillMemoryUlong, {0xB00, 12, 0x11223344});
  CHECK(GetBe32(0xB00) == 0x11223344);
  CHECK(GetBe32(0xB08) == 0x11223344);
  CHECK(Arena()[0xB00] == 0x11);  // high byte first
  PutBe32(0xB04, 0);
  CHECK(Call(__imp__RtlCompareMemoryUlong, {0xB00, 12, 0x11223344}) == 2);
  CHECK(Call(__imp__RtlCompareMemoryUlong, {0xB01, 8, 0x11223344}) == 0);  // unaligned address
  CHECK(Call(__imp__RtlCompareMemoryUlong, {0xB00, 10, 0x11223344}) == 0);  // length not a multiple of 4
}

// ---- errors -------------------------------------------------------------

TEST_CASE("RtlNtStatusToDosError: Xenia's table, direct pass-throughs and default 317") {
  CHECK(Call(__imp__RtlNtStatusToDosError, {0}) == 0);
  CHECK(Call(__imp__RtlNtStatusToDosError, {0x00000103}) == 0x3E5);       // STATUS_PENDING -> ERROR_IO_PENDING
  CHECK(Call(__imp__RtlNtStatusToDosError, {0x80070005}) == 5);           // Win32 HRESULT -> Win32 code
  CHECK(Call(__imp__RtlNtStatusToDosError, {0xC0000034}) == 2);           // OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND
  CHECK(Call(__imp__RtlNtStatusToDosError, {0xC0010007}) == 7);           // 0xC001xxxx -> xxxx
  CHECK(Call(__imp__RtlNtStatusToDosError, {0xC0FF0000}) == 317);         // unknown
}

// ---- time ---------------------------------------------------------------

TEST_CASE("RtlTimeToTimeFields and RtlTimeFieldsToTime: round trip on FILETIME") {
  // 2009-02-13 23:31:30.123 UTC (Friday) = unix 1234567890.123
  const uint64_t ft = (1234567890ull * 1000 + 123) * 10000 + 116444736000000000ull;
  PutBe64(0xC00, ft);
  Call(__imp__RtlTimeToTimeFields, {0xC00, 0xC10});
  CHECK(GetBe16(0xC10) == 2009);
  CHECK(GetBe16(0xC12) == 2);
  CHECK(GetBe16(0xC14) == 13);
  CHECK(GetBe16(0xC16) == 23);
  CHECK(GetBe16(0xC18) == 31);
  CHECK(GetBe16(0xC1A) == 30);
  CHECK(GetBe16(0xC1C) == 123);
  CHECK(GetBe16(0xC1E) == 5);  // 0 = Sunday

  PutBe64(0xC40, 0);
  CHECK(Call(__imp__RtlTimeFieldsToTime, {0xC10, 0xC40}) == 1);
  CHECK(GetBe64(0xC40) == ft);

  // FILETIME epoch: 1601-01-01 (Monday)
  PutBe64(0xC00, 0);
  Call(__imp__RtlTimeToTimeFields, {0xC00, 0xC10});
  CHECK(GetBe16(0xC10) == 1601);
  CHECK(GetBe16(0xC12) == 1);
  CHECK(GetBe16(0xC14) == 1);
  CHECK(GetBe16(0xC1E) == 1);

  // invalid fields: 0 and destination untouched
  PutBe16(0xC10, 2023); PutBe16(0xC12, 2); PutBe16(0xC14, 30);  // 30 February
  PutBe16(0xC16, 0); PutBe16(0xC18, 0); PutBe16(0xC1A, 0); PutBe16(0xC1C, 0);
  PutBe64(0xC40, 0x77);
  CHECK(Call(__imp__RtlTimeFieldsToTime, {0xC10, 0xC40}) == 0);
  CHECK(GetBe64(0xC40) == 0x77);
  PutBe16(0xC10, 1600); PutBe16(0xC12, 1); PutBe16(0xC14, 1);
  CHECK(Call(__imp__RtlTimeFieldsToTime, {0xC10, 0xC40}) == 0);
  PutBe16(0xC10, 2000); PutBe16(0xC12, 12); PutBe16(0xC14, 31); PutBe16(0xC16, 24);
  CHECK(Call(__imp__RtlTimeFieldsToTime, {0xC10, 0xC40}) == 0);
}

TEST_CASE("KeQuerySystemTime: FILETIME units and monotonic") {
  uint64_t prev = 0;
  for (int i = 0; i < 100; ++i) {
    Call(__imp__KeQuerySystemTime, {0xD00});
    const uint64_t t = GetBe64(0xD00);
    CHECK(t >= prev);
    prev = t;
  }
  // the current time as FILETIME lies between 2024 (1.33e17) and 2200 (1.66e17)
  CHECK(prev > 133'000'000'000'000'000ull);
  CHECK(prev < 166'000'000'000'000'000ull);
  // 100 ns units: a 50 ms wait advances by about 500 000 units
  Call(__imp__KeQuerySystemTime, {0xD00});
  const uint64_t a = GetBe64(0xD00);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Call(__imp__KeQuerySystemTime, {0xD00});
  const uint64_t d = GetBe64(0xD00) - a;
  CHECK(d >= 400'000);
  CHECK(d < 50'000'000);
}

TEST_CASE("plat::Ticks50MHz: monotono, 50 MHz") {
  const uint64_t a = plat::Ticks50MHz();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const uint64_t b = plat::Ticks50MHz();
  CHECK(b >= a);
  CHECK(b - a >= 2'000'000);    // 40 ms
  CHECK(b - a < 100'000'000);   // 2 s
}

// The guest timebase read by mftb (PPC_QUERY_TIMEBASE in runtime/ppc_detail.h).
extern "C" uint64_t gow2_query_timebase();

TEST_CASE("gow2_query_timebase: 50 MHz, monotonic, cheap") {
  // Busy wait of 100 ms on the steady clock (sleep_for on Windows may
  // overshoot by a scheduler quantum): 5'000'000 ticks +/- 10%.
  const auto start = std::chrono::steady_clock::now();
  const uint64_t a = gow2_query_timebase();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100)) {
  }
  const uint64_t b = gow2_query_timebase();
  CHECK(b - a >= 4'500'000);
  CHECK(b - a <= 5'500'000);

  // Monotonic over 1000 consecutive reads.
  uint64_t prev = gow2_query_timebase();
  bool monotonic = true;
  for (int i = 0; i < 1000; ++i) {
    const uint64_t now = gow2_query_timebase();
    if (now < prev) monotonic = false;
    prev = now;
  }
  CHECK(monotonic);

  // Same source as plat::Ticks50MHz (KeQueryPerformanceCounter): readings
  // comparable with each other.
  const uint64_t t0 = gow2_query_timebase();
  const uint64_t p = plat::Ticks50MHz();
  const uint64_t t1 = gow2_query_timebase();
  CHECK(t0 <= p);
  CHECK(p <= t1);

  // Cost per call: the game reads mftb in tight loops. Wide limit
  // (1 us), the measured value shows with `-s`.
  constexpr int kCalls = 1'000'000;
  uint64_t sink = 0;
  const auto c0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) sink += gow2_query_timebase();
  const auto c1 = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count() / kCalls;
  INFO("ns per call: " << ns);
  CHECK(sink != 0);
  CHECK(ns < 1000.0);
  SUCCEED("ns per call: " << ns);
}

TEST_CASE("Ke: 50 MHz frequency, title process type") {
  CHECK(Call(__imp__KeQueryPerformanceFrequency, {}) == 50'000'000);
  CHECK(Call(__imp__KeGetCurrentProcessType, {}) == 1);
}

TEST_CASE("XexCheckExecutablePrivilege: 0 without flags, system flags bit otherwise") {
  kernel::rtl::SetExecutableSystemFlags(0);
  for (uint32_t p = 0; p < 32; ++p) CHECK(Call(__imp__XexCheckExecutablePrivilege, {p}) == 0);
  kernel::rtl::SetExecutableSystemFlags(0x00000040);  // XEX_SYSTEM_INSECURE_SOCKETS (bit 6)
  CHECK(Call(__imp__XexCheckExecutablePrivilege, {6}) == 1);
  CHECK(Call(__imp__XexCheckExecutablePrivilege, {5}) == 0);
  CHECK(Call(__imp__XexCheckExecutablePrivilege, {40}) == 0);
  kernel::rtl::SetExecutableSystemFlags(0);
}

// ---- debug --------------------------------------------------------------

TEST_CASE("DbgPrint formats the arguments and logs without trailing whitespace") {
  const auto path = gow2_test::UniqueTempPath("gow2_rtl_dbgprint");
  LogInit(path);
  PutCString(0xE00, "val=%d hex=%08x s=%s %%\n");
  PutCString(0xE40, "hello");
  CHECK(Call(__imp__DbgPrint, {0xE00, uint64_t(-5) & 0xFFFFFFFF, 0xBEEF, 0xE40}) == 0);
  LogInit({});
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(path);
  CHECK(ss.str() == "[kernel] DbgPrint: val=-5 hex=0000beef s=hello %\n");
}

// ---- end of the process ---------------------------------------------------

TEST_CASE("KeBugCheck: child process exits with 4 and writes code and caller to the log") {
  const auto r = RunChild("bugcheck");
  CHECK(r.exit_code == 4);
  CHECK(Contains(r.log, "[kernel] KeBugCheck code=0x1234 caller=0x82001234"));
}

TEST_CASE("KeBugCheckEx: exit 4, code, caller and parameters in the log") {
  const auto r = RunChild("bugcheckex");
  CHECK(r.exit_code == 4);
  CHECK(Contains(r.log, "[kernel] KeBugCheck code=0xABCD caller=0x82005678"));
  CHECK(Contains(r.log, "params=0x1,0x2,0x3,0x4"));
}

TEST_CASE("HalReturnToFirmware: child process exits with 0 and writes the reason") {
  const auto r = RunChild("halreturn");
  CHECK(r.exit_code == 0);
  CHECK(Contains(r.log, "[kernel] HalReturnToFirmware routine=1"));
}

// ---- length-bounded conversions, XConfig, termination notifications -------

TEST_CASE("RtlMultiByteToUnicodeN / RtlUnicodeToMultiByteN: lengths in bytes, truncation, '?' beyond 0xFF") {
  // bytes -> UTF-16: dest 8 bytes = 4 characters, source 6: copies 4, written 8
  PutCString(0x100, "abcdef");
  std::memset(Arena() + 0x200, 0xEE, 16);
  CHECK(Call(__imp__RtlMultiByteToUnicodeN, {0x200, 8, 0x300, 0x100, 6}) == 0);
  CHECK(GetBe16(0x200) == 'a');
  CHECK(GetBe16(0x206) == 'd');
  CHECK(GetBe16(0x208) == 0xEEEE);  // nothing beyond dest_len
  CHECK(GetBe32(0x300) == 8);
  // null written allowed
  CHECK(Call(__imp__RtlMultiByteToUnicodeN, {0x200, 2, 0, 0x100, 6}) == 0);

  // UTF-16 -> bytes: source 8 bytes = 4 characters, dest 3: copies 3, written 3
  PutBe16(0x400, 'X');
  PutBe16(0x402, 0x00E9);  // < 256: copied
  PutBe16(0x404, 0x4E2D);  // >= 256: '?'
  PutBe16(0x406, 'Z');
  std::memset(Arena() + 0x500, 0xEE, 8);
  CHECK(Call(__imp__RtlUnicodeToMultiByteN, {0x500, 3, 0x600, 0x400, 8}) == 0);
  const uint8_t expected[4] = {'X', 0xE9, '?', 0xEE};
  CHECK(std::memcmp(Arena() + 0x500, expected, 4) == 0);
  CHECK(GetBe32(0x600) == 3);
  // a source address with dirty high 32 bits (sign extension) is truncated
  CHECK(Call(__imp__RtlUnicodeToMultiByteN, {0x500, 0x10, 0x600, 0xFFFFFFFF00000400ull, 8}) == 0);
  CHECK(GetBe32(0x600) == 4);
  CHECK(Arena()[0x503] == 'Z');
}

TEST_CASE("ExGetXConfigSetting: user and secured settings, requested size, errors") {
  // language (3, 9): big-endian u32 = XGetLanguage, required 4
  PutBe16(0x700, 0xFFFF);
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 9, 0x710, 4, 0x700}) == 0);
  CHECK(GetBe32(0x710) == 1);
  CHECK(GetBe16(0x700) == 4);
  // video flags and AV region
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 0xA, 0x710, 4, 0}) == 0);
  CHECK(GetBe32(0x710) == 0x00040000);
  CHECK(Call(__imp__ExGetXConfigSetting, {2, 2, 0x710, 4, 0}) == 0);
  CHECK(GetBe32(0x710) == 0x00001000);
  // country: one byte (103 = US)
  Arena()[0x720] = 0;
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 0xE, 0x720, 4, 0x700}) == 0);
  CHECK(Arena()[0x720] == 103);
  CHECK(GetBe16(0x700) == 1);
  // small buffer, null buffer with size, size only
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 9, 0x710, 2, 0}) == 0xC0000023);
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 9, 0, 4, 0}) == 0xC00000F1);
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 9, 0, 0, 0x700}) == 0);
  CHECK(GetBe16(0x700) == 4);
  // unknown category and setting
  CHECK(Call(__imp__ExGetXConfigSetting, {7, 1, 0x710, 4, 0}) == 0xC00000EF);
  CHECK(Call(__imp__ExGetXConfigSetting, {3, 0x55, 0x710, 4, 0}) == 0xC00000F0);
}

TEST_CASE("ExRegisterTitleTerminateNotification: registers and removes the routine") {
  const size_t before = kernel::rtl::TitleTerminateNotificationCount();
  PutBe32(0x800, 0x82001000);  // routine
  PutBe32(0x804, 5);           // priority
  Call(__imp__ExRegisterTitleTerminateNotification, {0x800, 1});
  CHECK(kernel::rtl::TitleTerminateNotificationCount() == before + 1);
  Call(__imp__ExRegisterTitleTerminateNotification, {0x800, 0});
  CHECK(kernel::rtl::TitleTerminateNotificationCount() == before);
}

// Body of the child process; hidden: runs only when requested by name/tag.
TEST_CASE("rtl child entry", "[.rtl_child]") {
  const char* mode = std::getenv("GOW2_RTL_TEST_CHILD");
  const char* log = std::getenv("GOW2_RTL_TEST_LOG");
  if (!mode || !log || !*mode) return;
  LogInit(log);
  if (std::strcmp(mode, "bugcheck") == 0) {
    Call(__imp__KeBugCheck, {0x1234}, 0x82001234);
  } else if (std::strcmp(mode, "bugcheckex") == 0) {
    Call(__imp__KeBugCheckEx, {0xABCD, 1, 2, 3, 4}, 0x82005678);
  } else if (std::strcmp(mode, "halreturn") == 0) {
    Call(__imp__HalReturnToFirmware, {1});
  }
  std::_Exit(99);  // the export did not terminate the process
}
