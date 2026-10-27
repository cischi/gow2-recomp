// Tests of the host -> guest map used by the crash handler (no game data).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "crash_map.h"

TEST_CASE("crash_map: host pc resolved to the guest function that contains it") {
  // deliberately not sorted by host address
  const std::vector<std::pair<uint32_t, uintptr_t>> table = {
      {0x82000100, 0x2000},
      {0x82000000, 0x1000},
  };
  CrashMapInitFrom(table);
  CHECK(GuestFunctionForHostPc(0x2004) == 0x82000100);
  CHECK(GuestFunctionForHostPc(0x2000) == 0x82000100);
  CHECK(GuestFunctionForHostPc(0x1000) == 0x82000000);
  CHECK(GuestFunctionForHostPc(0x1FFF) == 0x82000000);
  CHECK(GuestFunctionForHostPc(0x10) == 0);
}

TEST_CASE("crash_map: empty table") {
  CrashMapInitFrom({});
  CHECK(GuestFunctionForHostPc(0x2004) == 0);
}

TEST_CASE("crash_map: a new init replaces the previous one") {
  const std::vector<std::pair<uint32_t, uintptr_t>> a = {{0x82000000, 0x1000}};
  const std::vector<std::pair<uint32_t, uintptr_t>> b = {{0x83000000, 0x5000}};
  CrashMapInitFrom(a);
  CrashMapInitFrom(b);
  CHECK(GuestFunctionForHostPc(0x1004) == 0);
  CHECK(GuestFunctionForHostPc(0x5004) == 0x83000000);
}

TEST_CASE("crash_map: pc outside [first function, end of code] -> 0") {
  const std::vector<std::pair<uint32_t, uintptr_t>> table = {
      {0x82000000, 0x1000},
      {0x82000100, 0x2000},
  };
  CrashMapInitFrom(table, 0x3000);
  CHECK(GuestFunctionForHostPc(0x0FFF) == 0);
  CHECK(GuestFunctionForHostPc(0x2FFF) == 0x82000100);
  CHECK(GuestFunctionForHostPc(0x3000) == 0);
  CHECK(GuestFunctionForHostPc(0x7FF600000000) == 0);
}

TEST_CASE("crash_map: FormatCrashLine with known and unknown guest_func") {
  const std::vector<std::pair<uint32_t, uintptr_t>> table = {{0x82000000, 0x1000}};
  CrashMapInitFrom(table, 0x2000);
  char buf[256];

  plat::CrashInfo known{0x1004, 0xC0000005, true, 0x1'0000'1234};
  FormatCrashLine(buf, sizeof(buf), known, 0, 0);
  CHECK(std::string(buf) == "crash host=0x1004 guest_func=0x82000000 code=0xC0000005 addr=0x100001234");

  plat::CrashInfo unknown{0x9000, 0x80000003, false, 0};
  FormatCrashLine(buf, sizeof(buf), unknown, 0, 0);
  CHECK(std::string(buf) == "crash host=0x9000 guest_func=? code=0x80000003");
}

TEST_CASE("crash_map: access in the guest stack guard -> stack overflow") {
  const std::vector<std::pair<uint32_t, uintptr_t>> table = {{0x82000000, 0x1000}};
  CrashMapInitFrom(table, 0x2000);
  char buf[256];
  const uintptr_t guard_begin = 0x1'84CB'0000, guard_end = 0x1'84CC'0000;

  plat::CrashInfo in_guard{0x1004, 0xC0000005, true, guard_end - 8};
  FormatCrashLine(buf, sizeof(buf), in_guard, guard_begin, guard_end);
  CHECK(std::string(buf) ==
        "crash host=0x1004 guest_func=0x82000000 code=0xC0000005 addr=0x184CBFFF8 stack overflow");

  plat::CrashInfo above{0x1004, 0xC0000005, true, guard_end};
  FormatCrashLine(buf, sizeof(buf), above, guard_begin, guard_end);
  CHECK(std::string(buf).find("stack overflow") == std::string::npos);

  // truncated without leaving the buffer
  char small[16];
  FormatCrashLine(small, sizeof(small), in_guard, guard_begin, guard_end);
  CHECK(std::string(small) == "crash host=0x10");
}

TEST_CASE("crash_map: per-thread registered guards classify stack overflow") {
  const std::pair<uint32_t, uintptr_t> table[] = {{0x82000000, 0x1000}};
  CrashMapInitFrom(table, 0x2000);
  REQUIRE(RegisterStackGuard(0x5000'0000, 0x5001'0000));
  REQUIRE(RegisterStackGuard(0x6000'0000, 0x6001'0000));
  CHECK(InStackGuard(0x5000'0000));
  CHECK(InStackGuard(0x6000'FFFF));
  CHECK_FALSE(InStackGuard(0x6001'0000));
  CHECK_FALSE(RegisterStackGuard(0, 0x10));  // invalid
  char buf[160];
  plat::CrashInfo in_second{0x1004, 0xC0000005, true, 0x6000'0010};
  FormatCrashLine(buf, sizeof(buf), in_second);
  CHECK(std::string(buf) == "crash host=0x1004 guest_func=0x82000000 code=0xC0000005 addr=0x60000010 stack overflow");
  UnregisterStackGuard(0x6000'0000);
  CHECK_FALSE(InStackGuard(0x6000'0010));
  FormatCrashLine(buf, sizeof(buf), in_second);
  CHECK(std::string(buf).find("stack overflow") == std::string::npos);
  UnregisterStackGuard(0x5000'0000);
  CHECK_FALSE(InStackGuard(0x5000'0000));
}
