// XMA register hook on PPC_MM_STORE_U32 and the PPC_QUERY_TIMEBASE timebase
// (runtime/ppc_detail.h): the macros are redefined before XenonUtils' ppc_context.h,
// as in the recompiled code. gow2_mmio_store32 and
// gow2_query_timebase are fakes (they record the calls): the executable does not link
// gow2_kernel. Synthetic data only.
#define PPC_CONFIG_H_INCLUDED
#include "ppc_detail.h"
#include <ppc_context.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

struct HookCall {
  uint32_t address;
  uint32_t value;
};
std::vector<HookCall> g_hook_calls;

// 64 KiB window acting as guest memory for the region [region, region + 0x10000):
// `base` is chosen so that base + guest address falls in the buffer.
struct Window {
  alignas(16) std::array<uint8_t, 0x10000> bytes{};
  uint32_t region;
  explicit Window(uint32_t r) : region(r) {}
  uint8_t* Base() { return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(bytes.data()) - region); }
  const uint8_t* At(uint32_t guest_address) const { return bytes.data() + (guest_address - region); }
};

}  // namespace

extern "C" void gow2_mmio_store32(uint32_t guest_address, uint32_t value) {
  g_hook_calls.push_back({guest_address, value});
}

TEST_CASE("mmio_hook: stwbrx on the Kick register passes the natural value") {
  g_hook_calls.clear();
  auto w = std::make_unique<Window>(0x7FEA0000);
  uint8_t* base = w->Base();
  PPC_MM_STORE_U32(0x7FEA1940, __builtin_bswap32(0x00000005));
  REQUIRE(g_hook_calls.size() == 1);
  CHECK(g_hook_calls[0].address == 0x7FEA1940);
  CHECK(g_hook_calls[0].value == 5);
  const uint8_t expected[4] = {0x05, 0x00, 0x00, 0x00};
  CHECK(std::memcmp(w->At(0x7FEA1940), expected, 4) == 0);
}

TEST_CASE("mmio_hook: stwx on register 0x601 passes the natural value") {
  g_hook_calls.clear();
  auto w = std::make_unique<Window>(0x7FEA0000);
  uint8_t* base = w->Base();
  PPC_MM_STORE_U32(0x7FEA1804, 0x03000000);
  REQUIRE(g_hook_calls.size() == 1);
  CHECK(g_hook_calls[0].address == 0x7FEA1804);
  CHECK(g_hook_calls[0].value == 3);
  const uint8_t expected[4] = {0x03, 0x00, 0x00, 0x00};
  CHECK(std::memcmp(w->At(0x7FEA1804), expected, 4) == 0);
}

TEST_CASE("mmio_hook: GPU registers 0x7FC8xxxx, no call and same bytes as PPC_STORE_U32") {
  g_hook_calls.clear();
  auto mm = std::make_unique<Window>(0x7FC80000);
  auto plain = std::make_unique<Window>(0x7FC80000);
  {
    uint8_t* base = mm->Base();
    PPC_MM_STORE_U32(0x7FC80010, 0x11223344);
  }
  {
    uint8_t* base = plain->Base();
    PPC_STORE_U32(0x7FC80010, 0x11223344);
  }
  CHECK(g_hook_calls.empty());
  const uint8_t expected[4] = {0x11, 0x22, 0x33, 0x44};
  CHECK(std::memcmp(mm->At(0x7FC80010), expected, 4) == 0);
  CHECK(mm->bytes == plain->bytes);
  // just outside the XMA region, above and below: normal store
  auto below = std::make_unique<Window>(0x7FE90000);
  {
    uint8_t* base = below->Base();
    PPC_MM_STORE_U32(0x7FE9FFFC, 1);
  }
  auto above = std::make_unique<Window>(0x7FEB0000);
  {
    uint8_t* base = above->Base();
    PPC_MM_STORE_U32(0x7FEB0000, 1);
  }
  CHECK(g_hook_calls.empty());
}

TEST_CASE("mmio_hook: address and value evaluated only once") {
  g_hook_calls.clear();
  auto w = std::make_unique<Window>(0x7FEA0000);
  uint8_t* base = w->Base();
  uint32_t address = 0x7FEA1A40;
  uint32_t value = __builtin_bswap32(4);
  PPC_MM_STORE_U32(address++, value++);
  CHECK(address == 0x7FEA1A41);
  CHECK(value == __builtin_bswap32(4) + 1);
  REQUIRE(g_hook_calls.size() == 1);
  CHECK(g_hook_calls[0].address == 0x7FEA1A40);
  CHECK(g_hook_calls[0].value == 4);
}

namespace {
uint64_t g_timebase_calls = 0;
}  // namespace

// Fake like gow2_mmio_store32: counts the calls and returns a known value.
extern "C" uint64_t gow2_query_timebase() {
  ++g_timebase_calls;
  return 0x123456789ull + g_timebase_calls;
}

TEST_CASE("mftb: PPC_QUERY_TIMEBASE calls gow2_query_timebase, not __rdtsc") {
  g_timebase_calls = 0;
  const uint64_t t = PPC_QUERY_TIMEBASE();
  CHECK(g_timebase_calls == 1);
  CHECK(t == 0x123456789ull + 1);
}
