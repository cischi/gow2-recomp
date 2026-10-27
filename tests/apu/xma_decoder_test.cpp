// XMA decoder (runtime/apu/xma_decoder.*): Xenia XmaContext per slot,
// Kick/Lock/Clear from the registers, "XMA Decoder" worker, context listeners.
// GuestMemory arena and test kernel; contexts with synthetic fields (no game
// bytes). Each TEST_CASE starts and stops the decoder and releases the
// contexts it allocated.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "apu/xma_decoder.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/xma.h"
#include "log.h"
#include "temp_dir.h"
#include "xenia/apu/xma_context.h"
#include "xenia/apu/xma_context_new.h"
#include "xenia/apu/xma_context_old.h"
#include "xenia/memory.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    kernel::g_base = mem.base;
    REQUIRE(kernel::InitMemory(mem));
    REQUIRE(kernel::xma::Init(mem));
  }
};
Env& E() {
  static Env env;
  return env;
}

uint32_t ContextAddress(uint32_t index) { return kernel::xma::ContextArray() + index * kernel::xma::kContextSize; }

// Allocates contexts 0..n-1 (the kernel hands out the first free one).
void AllocateFirst(uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) REQUIRE(kernel::xma::AllocateContext() == ContextAddress(i));
}
void Release(std::initializer_list<uint32_t> indices) {
  for (uint32_t i : indices) CHECK(kernel::xma::ReleaseContext(ContextAddress(i)));
}

template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
  const auto end = Clock::now() + timeout;
  while (!pred()) {
    if (Clock::now() >= end) return pred();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

uint64_t SummaryValue(const std::string& key) {
  const std::string summary = kernel::stats::Summary();
  const std::string prefix = "[summary] " + key + "=";
  const size_t at = summary.find(prefix);
  REQUIRE(at != std::string::npos);
  return std::stoull(summary.substr(at + prefix.size()));
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
  return n;
}

// Fake Work: counts calls per context; `slow` = index whose Work lasts
// 50 ms (and at the end writes 0xAB into the context's first byte).
struct FakeWork {
  std::array<std::atomic<uint32_t>, kernel::xma::kContextCount> calls{};
  std::atomic<int> slow{-1};
  std::atomic<bool> in_slow_work{false};

  // Addresses taken beforehand: ContextArray() takes the kernel::xma mutex, which
  // ReleaseContext holds while waiting for the Work.
  uint32_t array = kernel::xma::ContextArray();

  apu::XmaWorkFn Fn() {
    return [this](uint32_t index) {
      calls[index].fetch_add(1);
      if (static_cast<int>(index) == slow.load()) {
        in_slow_work = true;
        std::this_thread::sleep_for(50ms);
        *kernel::GuestPtr<uint8_t>(array + index * kernel::xma::kContextSize) = 0xAB;
        in_slow_work = false;
      }
      return true;
    };
  }
};

// Stops the decoder and checks that it returns within 100 ms.
void StopWithin100ms() {
  const auto t0 = Clock::now();
  apu::StopXmaDecoder();
  CHECK(Clock::now() - t0 < 100ms);
}

xe::apu::XMA_CONTEXT_DATA ReadContext(uint32_t index) {
  return xe::apu::XMA_CONTEXT_DATA(kernel::GuestPtr<uint8_t>(ContextAddress(index)));
}
void WriteContext(uint32_t index, xe::apu::XMA_CONTEXT_DATA data) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  data.Store(kernel::GuestPtr<uint8_t>(ContextAddress(index)));
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
xe::apu::XMA_CONTEXT_DATA ZeroContext() {
  static const std::array<uint8_t, 64> zero{};
  return xe::apu::XMA_CONTEXT_DATA(zero.data());
}

}  // namespace

TEST_CASE("xma_decoder: Kick enables allocated contexts, Lock stops them") {
  E();
  FakeWork fake;
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  AllocateFirst(34);
  for (uint32_t i = 2; i < 33; ++i) Release({i});  // 0, 1 and 33 remain

  kernel::xma::OnRegisterWrite(0x650, 0b11);
  CHECK(WaitFor([&] { return fake.calls[0] > 0 && fake.calls[1] > 0; }, 100ms));
  CHECK(fake.calls[33] == 0);
  CHECK(apu::GetXmaContextState(1).allocated);
  CHECK(apu::GetXmaContextState(1).enabled);
  CHECK(apu::GetXmaContextState(33).allocated);
  CHECK_FALSE(apu::GetXmaContextState(33).enabled);

  kernel::xma::OnRegisterWrite(0x690, 0b10);
  CHECK_FALSE(apu::GetXmaContextState(1).enabled);
  CHECK(apu::GetXmaContextState(0).enabled);
  const uint32_t after_lock = fake.calls[1];
  std::this_thread::sleep_for(100ms);
  CHECK(fake.calls[1] == after_lock);
  CHECK(fake.calls[33] == 0);

  Release({0, 1, 33});
  CHECK_FALSE(apu::GetXmaContextState(0).allocated);
  CHECK_FALSE(apu::GetXmaContextState(0).enabled);
  StopWithin100ms();
  apu::StopXmaDecoder();  // idempotent
}

TEST_CASE("xma_decoder: Lock waits for the running Work to finish") {
  E();
  FakeWork fake;
  fake.slow = 0;
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  AllocateFirst(1);
  kernel::xma::OnRegisterWrite(0x650, 1);
  REQUIRE(WaitFor([&] { return fake.in_slow_work.load(); }, 100ms));

  const auto t0 = Clock::now();
  kernel::xma::OnRegisterWrite(0x690, 1);
  const auto waited = Clock::now() - t0;
  CHECK(waited >= 40ms);
  CHECK_FALSE(fake.in_slow_work);
  CHECK_FALSE(apu::GetXmaContextState(0).enabled);
  const uint32_t after_lock = fake.calls[0];
  std::this_thread::sleep_for(100ms);
  CHECK(fake.calls[0] == after_lock);

  Release({0});
  StopWithin100ms();
}

TEST_CASE("xma_decoder: Kick on an unallocated context is ignored but counted") {
  E();
  FakeWork fake;
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  AllocateFirst(1);
  const uint64_t kicks = SummaryValue("xma_kicks");
  kernel::xma::OnRegisterWrite(0x650, 1u << 5);
  kernel::xma::OnRegisterWrite(0x6A0, 1u << 5);  // Clear: ignored too
  kernel::xma::OnRegisterWrite(0x690, 1u << 5);
  std::this_thread::sleep_for(50ms);
  CHECK(fake.calls[5] == 0);
  CHECK(SummaryValue("xma_kicks") == kicks + 1);
  CHECK_FALSE(apu::GetXmaContextState(5).allocated);
  CHECK_FALSE(apu::GetXmaContextState(5).enabled);
  // Out-of-range indices: empty state.
  CHECK_FALSE(apu::GetXmaContextState(320).allocated);
  CHECK_FALSE(apu::GetXmaContextState(0xFFFFFFFF).enabled);

  Release({0});
  StopWithin100ms();
}

TEST_CASE("xma_decoder: ReleaseContext of an enabled context disables it before zeroing it") {
  E();
  FakeWork fake;
  fake.slow = 0;
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  AllocateFirst(1);
  kernel::xma::OnRegisterWrite(0x650, 1);
  REQUIRE(WaitFor([&] { return fake.in_slow_work.load(); }, 100ms));

  // The running Work writes 0xAB at the end: if the release did not wait for it,
  // the zeroing would come first and the byte would remain.
  REQUIRE(kernel::xma::ReleaseContext(ContextAddress(0)));
  CHECK_FALSE(fake.in_slow_work);
  CHECK_FALSE(apu::GetXmaContextState(0).allocated);
  CHECK_FALSE(apu::GetXmaContextState(0).enabled);
  const std::array<uint8_t, 64> zero{};
  CHECK(std::memcmp(kernel::GuestPtr<uint8_t>(ContextAddress(0)), zero.data(), 64) == 0);
  const uint32_t after_release = fake.calls[0];
  std::this_thread::sleep_for(100ms);
  CHECK(fake.calls[0] == after_release);

  StopWithin100ms();
}

TEST_CASE("xma_decoder: Clear resets the context to the values of XmaContextNew::Clear") {
  E();
  REQUIRE(apu::StartXmaDecoder(E().mem));
  AllocateFirst(1);
  // Output not valid: the real Work (Decode) does not touch the context.
  xe::apu::XMA_CONTEXT_DATA d = ZeroContext();
  d.input_buffer_0_packet_count = 3;
  d.input_buffer_0_valid = 1;
  d.input_buffer_1_valid = 1;
  d.output_buffer_block_count = 4;
  d.output_buffer_write_offset = 5;
  d.output_buffer_read_offset = 3;
  d.input_buffer_read_offset = 1234;
  d.input_buffer_0_ptr = 0x00100000;
  d.input_buffer_1_ptr = 0x00200000;
  d.output_buffer_ptr = 0x00300000;
  d.loop_end = 77;
  WriteContext(0, d);

  kernel::xma::OnRegisterWrite(0x650, 1);
  kernel::xma::OnRegisterWrite(0x6A0, 1);
  CHECK(WaitFor([] { return !apu::GetXmaContextState(0).enabled; }, 100ms));

  xe::apu::XMA_CONTEXT_DATA expected = d;
  expected.input_buffer_0_valid = 0;
  expected.input_buffer_1_valid = 0;
  expected.output_buffer_valid = 0;
  expected.output_buffer_read_offset = 0;
  expected.output_buffer_write_offset = 0;
  // XmaContextNew::ClearLocked also moves the input read position past the first packet's
  // header (kBitsPerPacketHeader = 32 bits); Xenia @95a5c3e's Clear left it as it was.
  expected.input_buffer_read_offset = 32;
  std::array<uint8_t, 64> want{};
  expected.Store(want.data());
  std::atomic_thread_fence(std::memory_order_seq_cst);
  CHECK(std::memcmp(kernel::GuestPtr<uint8_t>(ContextAddress(0)), want.data(), 64) == 0);

  Release({0});
  StopWithin100ms();
}

TEST_CASE("xma_decoder: real Work on hostile contexts does not crash") {
  E();
  gow2_test::TempDir dir("xma_decoder");
  const auto log_path = dir.path / "log.txt";
  LogInit(log_path);
  REQUIRE(apu::StartXmaDecoder(E().mem));
  AllocateFirst(3);
  // The physical page 0x1FFFF000 was never written: zero content.
  // (The physical heap allocates from the top: the page may host the array's
  // contexts 256..319, never allocated here and therefore zero.)
  const uint32_t array_phys = kernel::xma::ContextArray() & 0x1FFFFFFF;
  REQUIRE((array_phys + 3 * 64 <= 0x1FFFF000));
  {
    const uint8_t* page = kernel::GuestPtr<uint8_t>(0xA0000000u + 0x1FFFF000u);
    const std::array<uint8_t, 0x1000> zero{};
    REQUIRE(std::memcmp(page, zero.data(), zero.size()) == 0);
  }
  const uint32_t output = kernel::Memory()->phys.Alloc(0x2000, 0x1000);
  REQUIRE(output != 0);

  // Context 0: valid input on a page of zeros, valid output.
  xe::apu::XMA_CONTEXT_DATA d = ZeroContext();
  d.input_buffer_0_valid = 1;
  d.input_buffer_0_packet_count = 1;
  d.input_buffer_0_ptr = 0x1FFFF000;
  d.output_buffer_valid = 1;
  d.output_buffer_block_count = 8;
  d.output_buffer_ptr = output & 0x1FFFFFFF;
  // Context 1: null and out-of-range pointers, zero-capacity output.
  xe::apu::XMA_CONTEXT_DATA h = ZeroContext();
  h.input_buffer_0_valid = 1;
  h.input_buffer_1_valid = 1;
  h.input_buffer_0_packet_count = 4095;
  h.input_buffer_1_packet_count = 4095;
  h.input_buffer_0_ptr = 0;
  h.input_buffer_1_ptr = 0xFFFFF000;
  h.output_buffer_valid = 1;
  h.output_buffer_block_count = 0;
  h.output_buffer_ptr = 0xFFFFFFFF;
  // Context 2: 4095 packets from 0x1FFFF000 (past the end of the 512 MiB
  // physical memory, in the 0xC0000000 mirror), valid output.
  xe::apu::XMA_CONTEXT_DATA t = d;
  t.input_buffer_0_packet_count = 4095;
  t.input_buffer_1_valid = 1;
  t.input_buffer_1_ptr = 0x1FFFFFFF;
  t.input_buffer_1_packet_count = 4095;

  for (int kick = 0; kick < 10; ++kick) {
    WriteContext(0, d);
    WriteContext(1, h);
    WriteContext(2, t);
    kernel::xma::OnRegisterWrite(0x650, 0b111);
    CHECK(WaitFor(
        [] {
          return !apu::GetXmaContextState(0).enabled && !apu::GetXmaContextState(1).enabled &&
                 !apu::GetXmaContextState(2).enabled;
        },
        1000ms));
  }
  StopWithin100ms();
  LogInit({});

  std::ifstream in(log_path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string log = ss.str();
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 0\n") <= 1);
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 1\n") == 1);  // zero capacity
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 2\n") <= 1);

  REQUIRE(apu::StartXmaDecoder(E().mem));  // for the release listeners
  Release({0, 1, 2});
  apu::StopXmaDecoder();
  kernel::Memory()->phys.Free(output);
}

TEST_CASE("xma_decoder: StopXmaDecoder returns within 100 ms, Start after Stop restarts") {
  E();
  FakeWork fake;
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));  // second call: no effect
  std::this_thread::sleep_for(60ms);                  // worker at rest (20 ms waits)
  StopWithin100ms();
  StopWithin100ms();
  CHECK_FALSE(apu::GetXmaContextState(0).allocated);

  // After Stop the registers no longer reach the decoder.
  AllocateFirst(1);
  kernel::xma::OnRegisterWrite(0x650, 1);
  std::this_thread::sleep_for(30ms);
  CHECK(fake.calls[0] == 0);
  CHECK(kernel::xma::ReleaseContext(ContextAddress(0)));

  REQUIRE(apu::StartXmaDecoder(E().mem, fake.Fn()));
  AllocateFirst(1);
  kernel::xma::OnRegisterWrite(0x650, 1);
  CHECK(WaitFor([&] { return fake.calls[0] > 0; }, 100ms));
  Release({0});
  StopWithin100ms();
}

TEST_CASE("xma_decoder: ParseXmaDecoder accepts only lowercase new and old") {
  CHECK(apu::ParseXmaDecoder("new") == apu::XmaDecoderKind::kNew);
  CHECK(apu::ParseXmaDecoder("old") == apu::XmaDecoderKind::kOld);
  CHECK_FALSE(apu::ParseXmaDecoder("NEW").has_value());
  CHECK_FALSE(apu::ParseXmaDecoder("master").has_value());
  CHECK_FALSE(apu::ParseXmaDecoder("").has_value());
  CHECK(apu::XmaDecoderName(apu::XmaDecoderKind::kNew) == std::string_view("new"));
  CHECK(apu::XmaDecoderName(apu::XmaDecoderKind::kOld) == std::string_view("old"));
}

namespace {
// Writes `value` (`bits` bits, big-endian) starting at bit `bit` of `p`.
void PutBits(uint8_t* p, uint32_t bit, uint32_t bits, uint32_t value) {
  for (uint32_t i = 0; i < bits; ++i) {
    const uint32_t b = bit + i;
    const uint8_t mask = uint8_t(0x80 >> (b & 7));
    if ((value >> (bits - 1 - i)) & 1) p[b / 8] |= mask;
    else p[b / 8] &= uint8_t(~mask);
  }
}
}  // namespace

TEST_CASE("xma_decoder: an infinitely looping stream does not block the worker") {
  E();
  gow2_test::TempDir dir("xma_decoder");
  LogInit(dir.path / "log.txt");
  REQUIRE(apu::StartXmaDecoder(E().mem));
  AllocateFirst(1);
  // Two 2048-byte XMA2 packets, one frame each 1000 bits long (15-bit header +
  // pseudo-random bits), rest of the packet set to 1 (no other frame). Loop of 255 passes from
  // 32 (first frame) to the end of the second packet's frame: if the frames produce no
  // audio the offset changes on every pass and Work's "no progress" check does not trigger.
  constexpr uint32_t kPacket = 2048, kFrameBits = 1000;
  const uint32_t input = kernel::Memory()->phys.Alloc(2 * kPacket, 0x1000);
  const uint32_t output = kernel::Memory()->phys.Alloc(0x2000, 0x1000);
  REQUIRE(input != 0);
  REQUIRE(output != 0);
  uint8_t* in = kernel::GuestPtr<uint8_t>(0xA0000000u + (input & 0x1FFFFFFF));
  uint32_t seed = 12345;
  for (uint32_t k = 0; k < 2; ++k) {
    uint8_t* p = in + k * kPacket;
    std::memset(p, 0xFF, kPacket);
    p[0] = 1 << 2;  // 1 frame, offset of the first frame 0 (right after the header)
    p[1] = 0;
    p[2] = 1;       // metadata 1 = XMA2
    p[3] = 0;       // no packet to skip
    PutBits(p, 32, 15, kFrameBits);
    for (uint32_t b = 47; b < 32 + kFrameBits; ++b) {
      seed = seed * 1103515245u + 12345u;
      PutBits(p, b, 1, (seed >> 16) & 1);
    }
  }
  xe::apu::XMA_CONTEXT_DATA d = ZeroContext();
  d.input_buffer_0_valid = 1;
  d.input_buffer_0_packet_count = 2;
  d.input_buffer_0_ptr = input & 0x1FFFFFFF;
  d.input_buffer_read_offset = 32;
  d.output_buffer_valid = 1;
  d.output_buffer_block_count = 8;
  d.output_buffer_ptr = output & 0x1FFFFFFF;
  d.subframe_decode_count = 1;
  d.loop_count = 255;
  d.loop_start = 32;
  d.loop_end = kPacket * 8 + 32 + kFrameBits;
  WriteContext(0, d);
  kernel::xma::OnRegisterWrite(0x650, 1);
  // Work must finish (worker free) within 2 s, whatever it decodes.
  CHECK(WaitFor([] { return !apu::GetXmaContextState(0).enabled; }, 2000ms));
  const auto t0 = Clock::now();
  CHECK(WaitFor([] { return apu::GetXmaContextState(0).allocated; }, 10ms));
  Release({0});
  CHECK(Clock::now() - t0 < 2000ms);
  StopWithin100ms();
  LogInit({});
}

// Same hostile contexts as the test above, but Work called directly on an XmaContextNew and on an
// XmaContextOld (--xma-decoder old): the process-wide decoder stays of a single type.
TEMPLATE_TEST_CASE("xma_decoder: hostile contexts on every context type, without crashing and with the error counted", "",
                   xe::apu::XmaContextNew, xe::apu::XmaContextOld) {
  E();
  gow2_test::TempDir dir("xma_decoder");
  LogInit(dir.path / "log.txt");
  const uint32_t output = kernel::Memory()->phys.Alloc(0x2000, 0x1000);
  const uint32_t ctx = kernel::Memory()->phys.Alloc(0x1000, 0x1000);
  REQUIRE(output != 0);
  REQUIRE(ctx != 0);
  const uint32_t ctx_virt = 0xA0000000u + (ctx & 0x1FFFFFFF);
  xe::Memory memory(E().mem.base);
  TestType context;
  REQUIRE(context.Setup(7, &memory, ctx_virt) == 0);
  context.set_is_allocated(true);

  xe::apu::XMA_CONTEXT_DATA zero_input = ZeroContext();  // input on a page of zeros
  zero_input.input_buffer_0_valid = 1;
  zero_input.input_buffer_0_packet_count = 1;
  zero_input.input_buffer_0_ptr = 0x1FFFF000;
  zero_input.output_buffer_valid = 1;
  zero_input.output_buffer_block_count = 8;
  zero_input.output_buffer_ptr = output & 0x1FFFFFFF;
  xe::apu::XMA_CONTEXT_DATA wild = ZeroContext();  // null/out-of-range pointers, zero-capacity output
  wild.input_buffer_0_valid = 1;
  wild.input_buffer_1_valid = 1;
  wild.input_buffer_0_packet_count = 4095;
  wild.input_buffer_1_packet_count = 4095;
  wild.input_buffer_1_ptr = 0xFFFFF000;
  wild.output_buffer_valid = 1;
  wild.output_buffer_block_count = 0;
  wild.output_buffer_ptr = 0xFFFFFFFF;
  xe::apu::XMA_CONTEXT_DATA tail = zero_input;  // 4095 packets past the end of the physical memory
  tail.input_buffer_0_packet_count = 4095;
  tail.input_buffer_1_valid = 1;
  tail.input_buffer_1_ptr = 0x1FFFFFFF;
  tail.input_buffer_1_packet_count = 4095;

  const uint64_t errors_before = SummaryValue("xma_decode_errors");
  for (auto d : {zero_input, wild, tail})
    for (int kick = 0; kick < 3; ++kick) {
      d.Store(kernel::GuestPtr<uint8_t>(ctx_virt));
      context.Enable();
      context.Work();
      CHECK_FALSE(context.is_enabled());
    }
  // Zero-capacity output counts one error per Work (patch 0008).
  CHECK(SummaryValue("xma_decode_errors") >= errors_before + 3);
  LogInit({});
  kernel::Memory()->phys.Free(ctx);
  kernel::Memory()->phys.Free(output);
}
