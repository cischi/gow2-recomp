// Guest -> host translation with the 4 KB shift of the 0xE0000000 view
// (runtime/guest_address.h) and the kernel translators that use it
// (kernel::GuestPtr / GuestAddr). No real arena: g_base is a fake base,
// pointers are compared without being dereferenced.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "guest_address.h"
#include "kernel/guest.h"

using gow2::GuestFromHostOffset;
using gow2::HostOffset;
using gow2::PhysicalAddressOf;

static_assert(gow2::kViewEBegin == 0xE0000000u);
static_assert(gow2::kViewEOffset == 0x1000u);

TEST_CASE("guest_address: HostOffset shifts only the 0xE view by 4 KB") {
  CHECK(HostOffset(0x82000000u) == 0x82000000ull);
  CHECK(HostOffset(0xDFFFFFFFu) == 0xDFFFFFFFull);
  CHECK(HostOffset(0xE0000000u) == 0xE0001000ull);
  CHECK(HostOffset(0xFFFFFFFFu) == 0x100000FFFull);
}

TEST_CASE("guest_address: GuestFromHostOffset inverts HostOffset") {
  for (uint32_t a : {0x0u, 0x82000000u, 0xA0001234u, 0xE0000000u, 0xE1234567u, 0xFFFFF000u})
    CHECK(GuestFromHostOffset(HostOffset(a)) == a);
}

TEST_CASE("guest_address: PhysicalAddressOf of the three physical views") {
  CHECK(PhysicalAddressOf(0xA1234567u) == 0x01234567u);
  CHECK(PhysicalAddressOf(0xC1234567u) == 0x01234567u);
  CHECK(PhysicalAddressOf(0xE1234567u) == 0x01235567u);
}

TEST_CASE("guest_address: GuestPtr and GuestAddr apply the 0xE view shift") {
  uint8_t* const saved = kernel::g_base;
  // Fake base (1 TB), never dereferenced: the addresses are compared.
  constexpr uintptr_t kFakeBase = uintptr_t{1} << 40;
  kernel::g_base = reinterpret_cast<uint8_t*>(kFakeBase);
  CHECK(reinterpret_cast<uintptr_t>(kernel::GuestPtr<uint8_t>(0xE0000010u)) == kFakeBase + 0xE0001010u);
  CHECK(kernel::GuestAddr(kernel::GuestPtr<uint8_t>(0xE0000010u)) == 0xE0000010u);
  CHECK(reinterpret_cast<uintptr_t>(kernel::GuestPtr<uint8_t>(0xA0000010u)) == kFakeBase + 0xA0000010u);
  CHECK(kernel::GuestAddr(kernel::GuestPtr<uint8_t>(0xA0000010u)) == 0xA0000010u);
  CHECK(kernel::GuestPtr<uint8_t>(0) == nullptr);
  CHECK(kernel::GuestAddr(nullptr) == 0);
  kernel::g_base = saved;
}
