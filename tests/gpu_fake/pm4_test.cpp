// Mini PM4 interpreter (runtime/gpu_fake/pm4.*): synthetic rings in a real
// GuestMemory arena, in the physical heap region; no game bytes.
#include "gpu_fake/pm4.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

#include "guest_memory.h"

namespace {

using namespace gpu_fake;

// Test physical pages: guest 0xA0100000 = physical 0x00100000.
constexpr uint32_t kPhys = 0x00100000;
constexpr uint32_t kGuest = 0xA0100000;
constexpr uint32_t kRing = kGuest;              // ring
constexpr uint32_t kIb1 = kGuest + 0x1000;      // indirect buffers
constexpr uint32_t kIb2 = kGuest + 0x2000;
constexpr uint32_t kData = kGuest + 0x8000;     // write targets
constexpr uint32_t kDataPhys = kPhys + 0x8000;
constexpr uint32_t kEndian8in32 = 2;            // Xenia Endian::k8in32

struct Arena {
  GuestMemory mem;
  Arena() {
    REQUIRE(mem.Init());
    REQUIRE(mem.Commit(kGuest, 0x10000));
  }
};

Arena& A() {
  static Arena a;
  return a;
}

uint8_t* Base() { return A().mem.base; }

void PutBe(uint32_t guest, uint32_t v) {
  const uint32_t be = std::byteswap(v);
  std::memcpy(Base() + guest, &be, 4);
}
uint32_t GetBe(uint32_t guest) {
  uint32_t v;
  std::memcpy(&v, Base() + guest, 4);
  return std::byteswap(v);
}
uint32_t GetRaw(uint32_t guest) {
  uint32_t v;
  std::memcpy(&v, Base() + guest, 4);
  return v;
}

// Writes the DWORDs starting at index `at` of a buffer of `size` DWORDs (wrap).
uint32_t Emit(uint32_t buffer, uint32_t size, uint32_t at, std::initializer_list<uint32_t> words) {
  for (uint32_t w : words) {
    PutBe(buffer + (at % size) * 4, w);
    ++at;
  }
  return at % size;
}

void Clear() { std::memset(Base() + kGuest, 0, 0x10000); }

// Effects that record everything; registers in a map.
struct Recorder {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<uint32_t> interrupts;
  std::vector<std::array<uint32_t, 3>> swaps;
  uint32_t counter = 0;
  Pm4State state;
  Pm4Effects Effects() {
    Pm4Effects e;
    e.write_reg = [this](uint32_t r, uint32_t v) {
      regs[r] = v;
      writes.emplace_back(r, v);
    };
    e.read_reg = [this](uint32_t r) { return regs.count(r) ? regs[r] : 0u; };
    e.interrupt = [this](uint32_t cpu) { interrupts.push_back(cpu); };
    e.swap = [this](uint32_t fb, uint32_t w, uint32_t h) { swaps.push_back({fb, w, h}); };
    e.counter = [this] { return counter; };
    e.state = &state;
    return e;
  }
};

constexpr uint32_t kRingSize = 1024;

uint32_t Run(Recorder& r, uint32_t read, uint32_t write, uint32_t size = kRingSize) {
  return ExecuteRing(Base(), kRing, size, read, write, r.Effects());
}

}  // namespace

TEST_CASE("pm4: MEM_WRITE writes to guest memory with the given swap", "[pm4]") {
  Clear();
  Recorder r;
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4MemWrite, 3), kDataPhys | kEndian8in32, 0x11223344, 0x55667788,
                     MakePacketType3(kPm4MemWrite, 2), (kDataPhys + 0x10), 0xAABBCCDD});
  CHECK(Run(r, 0, w) == w);
  CHECK(GetBe(kData) == 0x11223344);  // k8in32: big-endian as the CPU sees it
  CHECK(GetBe(kData + 4) == 0x55667788);
  CHECK(GetRaw(kData + 0x10) == 0xAABBCCDD);  // no swap: GPU byte order
  CHECK(r.state.counters.packets == 2);
  CHECK(r.state.counters.unknown_packets == 0);
}

TEST_CASE("pm4: EVENT_WRITE_SHD writes the fence (value or counter)", "[pm4]") {
  Clear();
  Recorder r;
  r.counter = 77;
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4EventWriteShd, 3), 0x00000007, kDataPhys | kEndian8in32, 0xFE11CE01,
                     MakePacketType3(kPm4EventWriteShd, 3), 0x80000007, (kDataPhys + 8) | kEndian8in32, 0x12345678});
  CHECK(Run(r, 0, w) == w);
  CHECK(GetBe(kData) == 0xFE11CE01);
  CHECK(GetBe(kData + 8) == 77);  // bit 31: counter
  CHECK(r.regs[kRegVgtEventInitiator] == 7);
}

TEST_CASE("pm4: an already satisfied WAIT_REG_MEM (memory and register) proceeds", "[pm4]") {
  Clear();
  Recorder r;
  PutBe(kData, 0x42);
  r.regs[0x0578] = 0x5;
  // memory: (value & 0xFF) == 0x42; register: (value & 0xF) >= 5
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4WaitRegMem, 5), 0x13, kDataPhys | kEndian8in32, 0x42, 0xFF, 0x100,
                     MakePacketType3(kPm4WaitRegMem, 5), 0x05, 0x0578, 0x5, 0xF, 0x100,
                     MakePacketType3(kPm4MemWrite, 2), (kDataPhys + 4) | kEndian8in32, 0xD0E});
  CHECK(Run(r, 0, w) == w);
  CHECK(GetBe(kData + 4) == 0xD0E);
}

TEST_CASE("pm4: an unsatisfied WAIT_REG_MEM stops when running() becomes false", "[pm4]") {
  Clear();
  Recorder r;
  int polls = 0;
  Pm4Effects e = r.Effects();
  e.running = [&] { return ++polls < 3; };
  uint32_t w = Emit(kRing, kRingSize, 0, {MakePacketType3(kPm4WaitRegMem, 5), 0x13, kDataPhys, 1, 0xFF, 0x100});
  CHECK(ExecuteRing(Base(), kRing, kRingSize, 0, w, e) == w);
  CHECK(polls >= 3);
}

// wait = 0x00FFFFFF: Xenia would sleep wait/0x100 ms (~65 s) between two reads;
// the wait goes in steps of at most 1 ms with running() checked at every step.
TEST_CASE("pm4: gpu WAIT_REG_MEM with a long wait stops within 50 ms when running() becomes false",
          "[pm4]") {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  Clear();
  Recorder r;
  Pm4Effects e = r.Effects();
  const auto start = Clock::now();
  e.running = [&] { return Clock::now() - start < 20ms; };
  uint32_t w =
      Emit(kRing, kRingSize, 0, {MakePacketType3(kPm4WaitRegMem, 5), 0x13, kDataPhys, 1, 0xFF, 0x00FFFFFF});
  CHECK(ExecuteRing(Base(), kRing, kRingSize, 0, w, e) == w);
  const auto elapsed = Clock::now() - start;
  const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
  INFO("ExecuteRing returned after " << elapsed_ms << " ms");
  CHECK(elapsed >= 20ms);
  CHECK(elapsed < 50ms);
}

TEST_CASE("pm4: nested INDIRECT_BUFFER", "[pm4]") {
  Clear();
  Recorder r;
  // IB2: MEM_WRITE; IB1: IB2 and then another write; ring: IB1.
  Emit(kIb2, 64, 0, {MakePacketType3(kPm4MemWrite, 2), kDataPhys | kEndian8in32, 0x1B2});
  Emit(kIb1, 64, 0,
       {MakePacketType3(kPm4IndirectBufferPfd, 2), kIb2 /* CPU address: masked */, 3,
        MakePacketType3(kPm4MemWrite, 2), (kDataPhys + 4) | kEndian8in32, 0x1B1});
  uint32_t w = Emit(kRing, kRingSize, 0, {MakePacketType3(kPm4IndirectBuffer, 2), kPhys + 0x1000, 6});
  CHECK(Run(r, 0, w) == w);
  CHECK(GetBe(kData) == 0x1B2);
  CHECK(GetBe(kData + 4) == 0x1B1);
  CHECK(r.state.counters.packets == 4);
}

TEST_CASE("pm4: INTERRUPT calls the interrupt for every CPU in the mask", "[pm4]") {
  Clear();
  Recorder r;
  uint32_t w = Emit(kRing, kRingSize, 0, {MakePacketType3(kPm4Interrupt, 1), 0x5});
  CHECK(Run(r, 0, w) == w);
  CHECK(r.interrupts == std::vector<uint32_t>{0, 2});
}

TEST_CASE("pm4: XE_SWAP counted as a frame", "[pm4]") {
  Clear();
  Recorder r;
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4XeSwap, 4), kSwapSignature, 0x01C00000, 1280, 720, MakePacketType2(),
                     MakePacketType3(kPm4XeSwap, 4), kSwapSignature, 0x01C00000, 1280, 720});
  CHECK(Run(r, 0, w) == w);
  CHECK(r.state.counters.frames == 2);
  REQUIRE(r.swaps.size() == 2);
  CHECK(r.swaps[0] == std::array<uint32_t, 3>{0x01C00000, 1280, 720});
  // predicated packet: the swap is always skipped (Xenia)
  w = Emit(kRing, kRingSize, w, {MakePacketType3(kPm4XeSwap, 4, true), kSwapSignature, 0, 1, 1});
  Run(r, 11, w);
  CHECK(r.state.counters.frames == 2);
}

TEST_CASE("pm4: unknown opcode skipped by the right length, counted", "[pm4]") {
  Clear();
  Recorder r;
  // the DRAW_INDX body contains DWORDs that would look like packets
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4DrawIndx, 3), MakePacketType3(kPm4MemWrite, 2), kDataPhys, 0xBAD,
                     MakePacketType3(kPm4SetConstant, 1), 0, MakePacketType3(kPm4DrawIndx, 1), 0,
                     MakePacketType3(kPm4MemWrite, 2), (kDataPhys + 4) | kEndian8in32, 0x600D});
  CHECK(Run(r, 0, w) == w);
  CHECK(GetRaw(kData) == 0);
  CHECK(GetBe(kData + 4) == 0x600D);
  CHECK(r.state.counters.unknown_packets == 3);
  CHECK(r.state.unknown_by_opcode[kPm4DrawIndx] == 2);
  CHECK(r.state.unknown_by_opcode[kPm4SetConstant] == 1);
  CHECK(r.state.counters.packets == 4);
}

TEST_CASE("pm4: ring wrap-around", "[pm4]") {
  Clear();
  Recorder r;
  constexpr uint32_t size = 16;
  // MEM_WRITE straddling the end: header and address in 14-15, data in 0
  uint32_t w = Emit(kRing, size, 14,
                    {MakePacketType3(kPm4MemWrite, 2), kDataPhys | kEndian8in32, 0x3A9, MakePacketType2()});
  CHECK(w == 2);
  CHECK(Run(r, 14, w, size) == 2);
  CHECK(GetBe(kData) == 0x3A9);
  CHECK(r.state.counters.packets == 2);
}

TEST_CASE("pm4: a truncated packet stops execution without reading past write", "[pm4]") {
  Clear();
  Recorder r;
  Emit(kRing, kRingSize, 0, {MakePacketType3(kPm4MemWrite, 2), kDataPhys | kEndian8in32, 0x7});
  CHECK(Run(r, 0, 2) == 2);  // the data is missing
  CHECK(GetRaw(kData) == 0);
}

TEST_CASE("pm4: tipo 0/1, REG_RMW, COND_WRITE, REG_TO_MEM", "[pm4]") {
  Clear();
  Recorder r;
  r.regs[0x300] = 0xF0F0;
  r.regs[0x301] = 0x000F;
  PutBe(kData, 9);
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType0(0x100, 2), 0xA, 0xB,                           // 0x100, 0x101
                     MakePacketType0(0x200, 2, true), 0xC, 0xD,                     // 0x200 twice
                     (1u << 30) | (0x0F2u << 11) | 0x0F1u, 0x11, 0x22,              // type 1: 0x0F1, 0x0F2
                     MakePacketType3(kPm4RegRmw, 3), 0x300, 0xFF00, 0x0003,         // (0xF0F0 & 0xFF00) | 3
                     MakePacketType3(kPm4RegRmw, 3), 0xC0000301, 0x0300, 0x0100,    // & reg | reg
                     MakePacketType3(kPm4CondWrite, 6), 0x13, kDataPhys | kEndian8in32, 9, 0xFF, 0x400, 0x44,
                     MakePacketType3(kPm4CondWrite, 6), 0x113, kDataPhys | kEndian8in32, 9, 0xFF,
                     (kDataPhys + 8) | kEndian8in32, 0x55,
                     MakePacketType3(kPm4CondWrite, 6), 0x14, kDataPhys | kEndian8in32, 9, 0xFF, 0x401, 0x66,
                     MakePacketType3(kPm4RegToMem, 2), 0x100, (kDataPhys + 12) | kEndian8in32});
  CHECK(Run(r, 0, w) == w);
  CHECK(r.regs[0x100] == 0xA);
  CHECK(r.regs[0x101] == 0xB);
  CHECK(r.regs[0x200] == 0xD);
  CHECK_FALSE(r.regs.count(0x201));
  CHECK(r.regs[0x0F1] == 0x11);
  CHECK(r.regs[0x0F2] == 0x22);
  CHECK(r.regs[0x300] == 0xF003);
  CHECK(r.regs[0x301] == ((0x000F & 0xF003) | 0xA));  // & reg[0x300], | reg[0x100]
  CHECK(r.regs[0x400] == 0x44);                      // condition true: register
  CHECK(GetBe(kData + 8) == 0x55);                   // condition true: memory
  CHECK_FALSE(r.regs.count(0x401));                  // condition false
  CHECK(GetBe(kData + 12) == 0xA);
}

TEST_CASE("pm4: WAIT_REG_MEM on COHER_STATUS_HOST makes it coherent (status cleared)", "[pm4]") {
  Clear();
  Recorder r;
  // bit 31 set by hand: on the real GPU the register write sets it
  // (gpu.cpp CpWriteRegister, test in gpu_test)
  r.regs[kRegCoherStatusHost] = 0x83000000;
  // waits for status (bit 31) == 0
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4WaitRegMem, 5), 0x3, kRegCoherStatusHost, 0, 0x80000000, 0x100});
  CHECK(Run(r, 0, w) == w);
  CHECK(r.regs[kRegCoherStatusHost] == 0);
}

TEST_CASE("pm4: predicated packets and SET_BIN_*", "[pm4]") {
  Clear();
  Recorder r;
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4SetBinMaskLo, 1), 0x1, MakePacketType3(kPm4SetBinMaskHi, 1), 0x0,
                     MakePacketType3(kPm4SetBinSelectLo, 1), 0x2, MakePacketType3(kPm4SetBinSelectHi, 1), 0x0,
                     MakePacketType3(kPm4MemWrite, 2, true), kDataPhys | kEndian8in32, 0x1,  // skipped
                     MakePacketType3(kPm4SetBinSelect, 2), 0x0, 0x1,
                     MakePacketType3(kPm4MemWrite, 2, true), (kDataPhys + 4) | kEndian8in32, 0x2});
  CHECK(Run(r, 0, w) == w);
  CHECK(r.state.bin_mask == 1);
  CHECK(r.state.bin_select == 1);
  CHECK(GetRaw(kData) == 0);
  CHECK(GetBe(kData + 4) == 2);
}

TEST_CASE("pm4: EVENT_WRITE_EXT and EVENT_WRITE_ZPD like Xenia", "[pm4]") {
  Clear();
  Recorder r;
  // ZPD: sample structure with D3D's end-of-query marker
  const uint32_t counts = kData + 0x100;
  for (int i = 0; i < 8; ++i) PutBe(counts + i * 4, 0xFFFFFEED);
  r.regs[kRegRbSampleCountAddr] = kDataPhys + 0x100;
  uint32_t w = Emit(kRing, kRingSize, 0,
                    {MakePacketType3(kPm4EventWriteExt, 2), 0x1A, kDataPhys | 1 /* k8in16 */,
                     MakePacketType3(kPm4EventWriteZpd, 1), 0x15});
  CHECK(Run(r, 0, w) == w);
  // extents: min x 0, max x 8192>>3, min y 0, max y 8192>>3, min z 0, max z 1 (big-endian u16)
  const uint8_t* ext = Base() + kData;
  CHECK((ext[2] << 8 | ext[3]) == 1024);
  CHECK((ext[10] << 8 | ext[11]) == 1);
  uint32_t total_a, zpass_a, zfail_a;
  std::memcpy(&total_a, Base() + counts, 4);
  std::memcpy(&zfail_a, Base() + counts + 8, 4);
  std::memcpy(&zpass_a, Base() + counts + 16, 4);
  CHECK(total_a == kFakeSampleCount);  // little-endian (D3D's LE structure)
  CHECK(zpass_a == kFakeSampleCount);
  CHECK(zfail_a == 0);
  CHECK(r.regs[kRegVgtEventInitiator] == 0x15);
}
