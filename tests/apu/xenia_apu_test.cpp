// Vendored xenia-canary APU (thirdparty/xenia, xenia_apu library):
// XmaContextNew and XmaContextOld on a test arena with the xe::Memory shim,
// XMA register table, BitStream. Synthetic data only (zeroed contexts,
// hand-picked bytes).
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "xenia/apu/xma_context_new.h"
#include "xenia/apu/xma_context_old.h"
#include "xenia/apu/xma_register_file.h"
#include "xenia/base/bit_stream.h"
#include "xenia/memory.h"

namespace {

// Fake guest arena: TranslateVirtual(guest) = base + guest. The physical part
// (base + 0xA0000000) is not touched: with a zeroed context no buffer is
// valid and Decode returns before translating physical addresses.
struct Arena {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x4000, 0);
  xe::Memory memory{bytes.data()};
};

constexpr uint32_t kContextGuest = 0x1000;

}  // namespace

TEST_CASE("xenia_apu: XMA_CONTEXT_DATA takes 64 bytes") {
  STATIC_REQUIRE(sizeof(xe::apu::XMA_CONTEXT_DATA) == 64);
}

TEST_CASE("xenia_apu: Memory::TranslateVirtual is base + guest address") {
  Arena arena;
  CHECK(arena.memory.TranslateVirtual(kContextGuest) == arena.bytes.data() + kContextGuest);
  CHECK(arena.memory.TranslateVirtual<uint32_t*>(0x20) ==
        reinterpret_cast<uint32_t*>(arena.bytes.data() + 0x20));
}

TEMPLATE_TEST_CASE("xenia_apu: XmaContext on a zeroed context, Work only after Enable", "",
                   xe::apu::XmaContextNew, xe::apu::XmaContextOld) {
  Arena arena;
  TestType context;
  REQUIRE(context.Setup(0, &arena.memory, kContextGuest) == 0);
  CHECK(context.id() == 0);
  CHECK(context.guest_ptr() == kContextGuest);

  context.set_is_allocated(true);
  context.Enable();
  CHECK(context.is_enabled());
  CHECK(context.Work());
  CHECK_FALSE(context.is_enabled());

  const uint8_t* data = arena.bytes.data() + kContextGuest;
  std::array<uint8_t, 64> zero{};
  CHECK(std::memcmp(data, zero.data(), zero.size()) == 0);

  // Without a new Enable there is no work.
  CHECK_FALSE(context.Work());

  // Block with no decode in progress: returns immediately.
  CHECK(context.Block(true));
  // Release zeroes the context and marks it free; Work does nothing.
  context.Release();
  CHECK_FALSE(context.is_allocated());
  context.Enable();
  CHECK_FALSE(context.Work());
}

TEST_CASE("xenia_apu: XmaRegisterFile knows the Kick, Lock and Clear registers") {
  const xe::apu::XmaRegisterInfo* kick = xe::apu::XmaRegisterFile::GetRegisterInfo(0x650);
  REQUIRE(kick != nullptr);
  CHECK(std::string(kick->name) == "Context0Kick");
  const xe::apu::XmaRegisterInfo* lock = xe::apu::XmaRegisterFile::GetRegisterInfo(0x699);
  REQUIRE(lock != nullptr);
  CHECK(std::string(lock->name) == "Context9Lock");
  const xe::apu::XmaRegisterInfo* clear = xe::apu::XmaRegisterFile::GetRegisterInfo(0x6A0);
  REQUIRE(clear != nullptr);
  CHECK(std::string(clear->name) == "Context0Clear");
  CHECK(xe::apu::XmaRegisterFile::GetRegisterInfo(0x6FF) == nullptr);
}

TEST_CASE("xenia_apu: BitStream reads bits in big-endian order") {
  // Peek reads 8 bytes at a time: the buffer has slack beyond the 16 valid bits.
  std::array<uint8_t, 16> buffer{0xA5, 0x0F};
  xe::BitStream stream(buffer.data(), 16);
  CHECK(stream.Read(4) == 0xA);
  CHECK(stream.Read(8) == 0x50);
  CHECK(stream.offset_bits() == 12);
  CHECK(stream.BitsRemaining() == 4);
  CHECK(stream.Read(4) == 0xF);
}
