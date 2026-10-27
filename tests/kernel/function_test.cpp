// PPC -> C++ argument marshalling of runtime/kernel/function.h
// (KERNEL_EXPORT / GUEST_FUNCTION_HOOK) on a synthetic PPCContext and a scratch
// arena: no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>

#include "guest_memory.h"
#include "kernel/guest.h"

namespace {

// Scratch guest arena: guest address A lives at g_arena + A.
constexpr size_t kArenaSize = 0x10000;
uint8_t* Arena() {
  alignas(64) static uint8_t arena[kArenaSize];
  return arena;
}

void PutBe32(uint32_t addr, uint32_t v) {
  const be<uint32_t> b = v;
  std::memcpy(Arena() + addr, &b, sizeof(b));
}

uint32_t GetBe32(uint32_t addr) {
  be<uint32_t> b;
  std::memcpy(&b, Arena() + addr, sizeof(b));
  return b;
}

// Last call seen by the host functions.
struct Seen {
  uint32_t a = 0;
  be<uint32_t>* p = nullptr;
  uint32_t p_value = 0;
  uint64_t c = 0;
  double d = 0;
  std::array<uint32_t, 10> ten{};
} g_seen;

uint32_t HostFour(uint32_t a, be<uint32_t>* p, uint64_t c, double d) {
  g_seen.a = a;
  g_seen.p = p;
  g_seen.p_value = p ? uint32_t(*p) : 0;
  if (p) *p = 0xCAFEF00D;
  g_seen.c = c;
  g_seen.d = d;
  return 0x12345678;
}

uint32_t HostTen(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6,
                 uint32_t a7, uint32_t a8, uint32_t a9) {
  g_seen.ten = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};
  return a8 + a9;
}

}  // namespace

KERNEL_EXPORT(TestMarshalFour, HostFour)
KERNEL_EXPORT(TestMarshalTen, HostTen)

TEST_CASE("GuestRangeValid: ranges inside the 4 GiB guest space") {
  using kernel::GuestRangeValid;
  CHECK(GuestRangeValid(0, 0));
  CHECK(GuestRangeValid(0xFFFFFFFF, 0));
  CHECK(GuestRangeValid(0xFFFFFFFF, 1));
  CHECK_FALSE(GuestRangeValid(0xFFFFFFFF, 2));
  CHECK(GuestRangeValid(0x80000000, 0x80000000));
  CHECK_FALSE(GuestRangeValid(0x80000000, 0x80000001));
  CHECK(GuestRangeValid(0, 0x100000000ull));
  CHECK_FALSE(GuestRangeValid(0, 0x100000001ull));
}

TEST_CASE("GuestRangeValid: the last page of 0xE stays inside the tail reserve") {
  using kernel::GuestRangeValid;
  // With the 0x1000 offset of the 0xE view the host range of a valid guest
  // range ends at most at kSize + 0x1000: inside GuestMemory's uncommitted
  // tail reserve (an access is a fault, not host memory).
  CHECK(GuestRangeValid(0xFFFFF000u, 0x1000));
  CHECK_FALSE(GuestRangeValid(0xFFFFF000u, 0x1001));
  CHECK(gow2::HostOffset(0xFFFFF000u) + 0x1000 <= GuestMemory::kSize + GuestMemory::kTailReserve);
  CHECK(gow2::HostOffset(0xE0000000u) + (kernel::kGuestSpaceSize - 0xE0000000u) <=
        GuestMemory::kSize + GuestMemory::kTailReserve);
  CHECK_FALSE(GuestRangeValid(0x10, ~uint64_t(0)));
}

TEST_CASE("marshalling: GuestPtr/GuestAddr translate relative to g_base, 0 <-> nullptr") {
  kernel::g_base = Arena();
  CHECK(kernel::GuestPtr<uint32_t>(0) == nullptr);
  CHECK(kernel::GuestPtr<uint8_t>(0x1234) == Arena() + 0x1234);
  CHECK(kernel::GuestAddr(nullptr) == 0);
  CHECK(kernel::GuestAddr(Arena() + 0x4321) == 0x4321);
}

TEST_CASE("marshalling: integers, guest pointer, uint64 and double arrive translated; return in r3") {
  kernel::g_base = Arena();
  g_seen = {};
  PutBe32(0x2000, 0xDEADBEEF);

  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFFFFFF'00000007ull;  // dirty high half: a uint32_t discards it
  ctx.r4.u64 = 0x2000;                  // guest address
  ctx.r5.u64 = 0x1122334455667788ull;   // whole uint64_t in the register
  ctx.f1.f64 = 3.25;
  ctx.r1.u64 = 0x8000;

  __imp__TestMarshalFour(ctx, Arena());

  CHECK(g_seen.a == 7);
  CHECK(g_seen.p == reinterpret_cast<be<uint32_t>*>(Arena() + 0x2000));
  CHECK(g_seen.p_value == 0xDEADBEEF);
  CHECK(GetBe32(0x2000) == 0xCAFEF00D);  // big-endian write visible to the guest
  CHECK(g_seen.c == 0x1122334455667788ull);
  CHECK(g_seen.d == 3.25);
  CHECK(ctx.r3.u64 == 0x12345678);
}

TEST_CASE("marshalling: guest pointer 0 arrives as nullptr") {
  kernel::g_base = Arena();
  g_seen = {};
  PPCContext ctx{};
  ctx.r3.u64 = 1;
  ctx.r4.u64 = 0;
  __imp__TestMarshalFour(ctx, Arena());
  CHECK(g_seen.p == nullptr);
  CHECK(ctx.r3.u64 == 0x12345678);
}

TEST_CASE("marshalling: from the ninth argument on the guest stack is read (r1 + 0x54 + (i-8)*8)") {
  kernel::g_base = Arena();
  g_seen = {};
  PPCContext ctx{};
  ctx.r1.u64 = 0x8000;
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  for (int i = 0; i < 8; ++i) *regs[i] = 100 + i;
  PutBe32(0x8000 + 0x54 + 0 * 8, 108);
  PutBe32(0x8000 + 0x54 + 1 * 8, 109);
  // the high half of the 8-byte slot does not matter
  PutBe32(0x8000 + 0x50, 0xBAD0BAD0);
  PutBe32(0x8000 + 0x58, 0xBAD0BAD0);

  __imp__TestMarshalTen(ctx, Arena());

  for (int i = 0; i < 10; ++i) CHECK(g_seen.ten[i] == uint32_t(100 + i));
  CHECK(ctx.r3.u64 == 108 + 109);
}
