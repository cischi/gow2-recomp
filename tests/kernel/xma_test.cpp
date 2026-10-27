// "Deviceless" XMA decoder (runtime/kernel/xma.*): resting registers
// as the game reads them (lwbrx, little-endian), context array
// in the physical heap, XMACreateContext/XMAReleaseContext. Real
// GuestMemory arena; no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/xma.h"

PPC_EXTERN_FUNC(__imp__XMACreateContext);
PPC_EXTERN_FUNC(__imp__XMAReleaseContext);

namespace {

using namespace kernel;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
// Read like the game's lwbrx: little-endian DWORD.
uint32_t GetLe32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr<uint8_t>(a), 4);
  return v;
}

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

struct Env {
  GuestMemory mem;
  uint32_t scratch = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(xma::Init(mem));
    scratch = Memory()->phys.Alloc(0x1000, 0);
    REQUIRE(scratch != 0);
  }
};
Env& E() {
  static Env env;
  return env;
}

}  // namespace

TEST_CASE("xma: resting registers readable like lwbrx, physical ContextArrayAddress") {
  Env& e = E();
  const uint32_t array = xma::ContextArray();
  REQUIRE(array >= kPhysicalHeapBegin);
  CHECK(array % 256 == 0);
  // 0x7FEA1800 = register 0x600: the game's first access (sub_82951828)
  CHECK(GetLe32(xma::kRegisterBase + 0x1800) == (array & 0x1FFFFFFF));
  CHECK(xma::ReadRegister(xma::kRegContextArrayAddress) == (array & 0x1FFFFFFF));
  CHECK(GetLe32(xma::kRegisterBase + xma::kRegNextContextIndex * 4) == 1);
  // the rest is zero and the whole region is accessible (write included)
  CHECK(GetLe32(xma::kRegisterBase) == 0);
  CHECK(GetLe32(xma::kRegisterBase + 0xFFFC) == 0);
  xma::WriteRegister(xma::kRegContext0Kick, 0x80000001);
  CHECK(GetLe32(xma::kRegisterBase + xma::kRegContext0Kick * 4) == 0x80000001);
  xma::WriteRegister(xma::kRegContext0Kick, 0);
  CHECK(xma::ReadRegister(0x4000) == 0);  // outside the region
  (void)e;
}

TEST_CASE("xma: XMACreateContext from the array, 320 contexts, release zeroes and recycles") {
  Env& e = E();
  const uint32_t array = xma::ContextArray();
  const uint32_t out = e.scratch;
  std::set<uint32_t> got;
  for (uint32_t i = 0; i < xma::kContextCount; ++i) {
    REQUIRE(Call(__imp__XMACreateContext, {out}) == 0);
    const uint32_t c = Get32(out);
    CHECK(c >= array);
    CHECK(c < array + xma::kContextCount * xma::kContextSize);
    CHECK((c - array) % xma::kContextSize == 0);
    got.insert(c);
  }
  CHECK(got.size() == xma::kContextCount);
  // exhausted: 0 and STATUS_NO_MEMORY
  CHECK(Call(__imp__XMACreateContext, {out}) == 0xC0000017);
  CHECK(Get32(out) == 0);

  const uint32_t c = array + 5 * xma::kContextSize;
  std::memset(GuestPtr<uint8_t>(c), 0x5A, xma::kContextSize);  // the game fills the context
  CHECK(Call(__imp__XMAReleaseContext, {c}) == 0);
  CHECK(GuestPtr<uint8_t>(c)[0] == 0);
  CHECK(GuestPtr<uint8_t>(c)[63] == 0);
  CHECK(Call(__imp__XMACreateContext, {out}) == 0);
  CHECK(Get32(out) == c);
  // foreign addresses ignored
  CHECK_FALSE(xma::ReleaseContext(c + 4));
  CHECK_FALSE(xma::ReleaseContext(0x12345678));
  for (uint32_t a : got) CHECK(xma::ReleaseContext(a));
}

// Game writes to the registers (OnRegisterWrite, called by the hook of
// PPC_MM_STORE_U32): Kick/Lock/Clear to the listener with the register's first
// context and the bits, Kick counted in xma_kicks.
namespace {

struct ListenerCall {
  xma::RegisterOp op;
  uint32_t first_context;
  uint32_t bits;
};
std::vector<ListenerCall> g_calls;
void TestListener(xma::RegisterOp op, uint32_t first_context, uint32_t bits) {
  g_calls.push_back({op, first_context, bits});
}
// Value of a summary counter: the tests check its variation
// (the counters are global for the process and the TEST_CASEs may run in
// a single process, in any order).
uint64_t Counter(const std::string& name) {
  const std::string s = kernel::stats::Summary();
  const std::string key = "[summary] " + name + "=";
  const size_t pos = s.find(key);
  REQUIRE(pos != std::string::npos);
  return std::stoull(s.substr(pos + key.size()));
}

}  // namespace

TEST_CASE("xma: OnRegisterWrite notifies Kick, Lock and Clear to the listener") {
  Env& e = E();
  (void)e;
  g_calls.clear();
  xma::SetRegisterListener(&TestListener);
  const uint64_t kicks0 = Counter("xma_kicks");

  xma::OnRegisterWrite(0x651, 0x80000001);
  REQUIRE(g_calls.size() == 1);
  CHECK(g_calls[0].op == xma::RegisterOp::kKick);
  CHECK(g_calls[0].first_context == 32);
  CHECK(g_calls[0].bits == 0x80000001);
  CHECK(xma::ReadRegister(0x651) == 0x80000001);
  CHECK(Counter("xma_kicks") - kicks0 == 2);

  xma::OnRegisterWrite(0x690, 4);
  REQUIRE(g_calls.size() == 2);
  CHECK(g_calls[1].op == xma::RegisterOp::kLock);
  CHECK(g_calls[1].first_context == 0);
  CHECK(g_calls[1].bits == 4);

  xma::OnRegisterWrite(0x6A9, 1);
  REQUIRE(g_calls.size() == 3);
  CHECK(g_calls[2].op == xma::RegisterOp::kClear);
  CHECK(g_calls[2].first_context == 288);
  CHECK(g_calls[2].bits == 1);

  // control register (0x601, the game's stwx): only the register file
  xma::OnRegisterWrite(0x601, 3);
  CHECK(g_calls.size() == 3);
  CHECK(xma::ReadRegister(0x601) == 3);
  // Lock and Clear are not kicks
  CHECK(Counter("xma_kicks") - kicks0 == 2);
  xma::SetRegisterListener(nullptr);
}

TEST_CASE("xma: OnRegisterWrite without a listener counts the kick and does not crash") {
  Env& e = E();
  (void)e;
  xma::SetRegisterListener(nullptr);
  const uint64_t kicks0 = Counter("xma_kicks");
  xma::OnRegisterWrite(0x659, 0x7);
  xma::OnRegisterWrite(0x699, 0xFFFFFFFF);
  xma::OnRegisterWrite(0x6A0, 0xFFFFFFFF);
  CHECK(xma::ReadRegister(0x659) == 7);
  CHECK(Counter("xma_kicks") - kicks0 == 3);
}

TEST_CASE("xma: gow2_mmio_store32 translates the address into the register") {
  Env& e = E();
  (void)e;
  g_calls.clear();
  xma::SetRegisterListener(&TestListener);
  gow2_mmio_store32(0x7FEA1940, 5);  // register 0x650 = Kick of contexts 0..31
  REQUIRE(g_calls.size() == 1);
  CHECK(g_calls[0].op == xma::RegisterOp::kKick);
  CHECK(g_calls[0].first_context == 0);
  CHECK(g_calls[0].bits == 5);
  xma::SetRegisterListener(nullptr);
}

// Unaligned store in the region (the hardware would not accept it): the normal
// store stays (PPC_MM_STORE_U32 does it before the hook), the hook does not.
TEST_CASE("xma: gow2_mmio_store32 ignores unaligned addresses") {
  Env& e = E();
  (void)e;
  g_calls.clear();
  xma::SetRegisterListener(&TestListener);
  const uint64_t kicks0 = Counter("xma_kicks");
  const uint32_t before = xma::ReadRegister(0x650);
  for (uint32_t misalign : {1u, 2u, 3u}) gow2_mmio_store32(0x7FEA1940 + misalign, 0xFFFFFFFF);
  CHECK(g_calls.empty());
  CHECK(Counter("xma_kicks") == kicks0);
  CHECK(xma::ReadRegister(0x650) == before);
  xma::SetRegisterListener(nullptr);
}
