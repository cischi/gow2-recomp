// Xenia CommandProcessor (vendored, with the gow2 patches) on the guest ring and
// registers, without a GPU: gpu::CreateXeniaBackend backend with
// NullCommandProcessor (tests/gpu/null_command_processor.h) on the common
// GPU module (gpu::Init: registers at 0x7FC80000, vblank, interrupts). Real
// GuestMemory arena with the kernel initialized, "guest functions" in a
// fake function table (like tests/gpu_fake/gpu_test.cpp); no game bytes.
//
// Register/ring bridge: write pointer from CP_RB_WPTR in the guest
// region, read pointer in CP_RB_RPTR and at the write-back address,
// WAIT_REG_MEM on registers written by the CPU, interrupt on a guest thread.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include "gpu/gpu_system.h"
#include "gpu/null_command_processor.h"
#include "gpu/xenia_backend.h"
#include "gpu/xenos.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/threads.h"
#include "log.h"
#include "temp_dir.h"
#include "xenia/base/byte_order.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/memory.h"

// Patch 0002 (gow2-endian-msvc): this file is C++23 and uses the STL's std::endian;
// the polyfill in Xenia's code (C++17) must have the same values,
// verified in tests/gpu/xenia_core_test.cpp.
#ifndef __cpp_lib_endian
#error "xenia_cp_test must compile in C++20 or later (STL std::endian)"
#endif
static_assert(int(std::endian::little) == 0 && int(std::endian::big) == 1);
static_assert(std::endian::native == std::endian::little);

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSetGraphicsInterruptCallback);
PPC_EXTERN_FUNC(__imp__VdSwap);

namespace {

using namespace kernel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using gpu::kRegisterBase;
using gpu::MakePacketType0;
using gpu::MakePacketType2;
using gpu::MakePacketType3;

constexpr uint32_t kRegScratchReg3 = 0x057B;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }

template <class F>
bool Eventually(F cond, std::chrono::milliseconds limit = 3000ms) {
  const auto end = Clock::now() + limit;
  while (!cond()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Fake stack for the arguments beyond r10 (r1 + 0x54 + (i-8)*8).
uint32_t g_stack = 0;

// Calls an export like the generated code: arguments in r3..r10, then stack.
uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  ctx.r1.u64 = g_stack;
  size_t i = 0;
  for (uint64_t a : args) {
    if (i < 8)
      *regs[i] = a;
    else
      Put32(g_stack + 0x54 + uint32_t(i - 8) * 8, uint32_t(a));
    ++i;
  }
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

// ---- fake interrupt callback ----------------------------------------------------

constexpr uint32_t kFnInterrupt = 0x82000100;
std::atomic<int> g_cp_interrupts{0};
std::atomic<uint32_t> g_cp_user_data{0};
std::atomic<bool> g_cp_on_guest_thread{true};

PPC_FUNC(FnInterrupt) {
  if (ctx.r3.u32 != 1) return;  // vblank (source 0)
  if (!GuestThread::Current()) g_cp_on_guest_thread = false;
  g_cp_user_data = ctx.r4.u32;
  ++g_cp_interrupts;
}

PPCFunc* Resolve(uint32_t guest) { return guest == kFnInterrupt ? &FnInterrupt : nullptr; }

struct Env {
  GuestMemory mem;
  std::unique_ptr<xe::Memory> xenia_memory;
  gow2::test::NullCommandProcessor* cp = nullptr;
  uint32_t ring = 0, ring_phys = 0, wb = 0, wb_phys = 0, data = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(InitThreads(mem, &Resolve, 0x40000));
    xenia_memory = std::make_unique<xe::Memory>(mem.base);
    auto graphics_system = std::make_unique<xe::gpu::GraphicsSystem>(xenia_memory.get(), nullptr, nullptr);
    auto backend = gpu::CreateXeniaBackend(
        std::move(graphics_system),
        [this](xe::gpu::GraphicsSystem* gs) -> std::unique_ptr<xe::gpu::CommandProcessor> {
          auto p = std::make_unique<gow2::test::NullCommandProcessor>(gs);
          cp = p.get();
          return p;
        });
    REQUIRE(backend);
    REQUIRE(cp);
    REQUIRE(gpu::Init(mem, std::move(backend)));
    g_stack = mem.Alloc(0x1000, 0x10);
    REQUIRE(g_stack);
    // 32 KB ring (size_log2 12) and write-back in the physical heap, like the game
    ring = Memory()->phys.Alloc(0x8000, 0x1000);
    wb = Memory()->phys.Alloc(0x1000, 0x1000);
    data = Memory()->phys.Alloc(0x1000, 0x1000);
    REQUIRE((ring && wb && data));
    ring_phys = ring & 0x1FFFFFFF;
    wb_phys = wb & 0x1FFFFFFF;
    // each TEST_CASE runs in its own process (catch_discover_tests): ring ready here
    Call(__imp__VdInitializeRingBuffer, {ring_phys, 12});
    Call(__imp__VdEnableRingBufferRPtrWriteBack, {wb_phys, 6});
  }
  ~Env() { gpu::Shutdown(); }
};

Env& E() {
  static Env env;
  return env;
}

constexpr uint32_t kRingDwords = 0x8000 / 4;
uint32_t g_wptr = 0;

void Emit(std::initializer_list<uint32_t> words) {
  for (uint32_t w : words) {
    Put32(E().ring + g_wptr * 4, w);
    g_wptr = (g_wptr + 1) % kRingDwords;
  }
}

void Kick() { Put32(kRegisterBase + gpu::kRegCpRbWptr * 4, g_wptr); }

bool RingDrained(std::chrono::milliseconds limit) {
  return Eventually([] { return Get32(E().wb) == g_wptr; }, limit);
}

uint32_t Phys(uint32_t guest, uint32_t endian = 2) { return (guest & 0x1FFFFFFF) | endian; }

// Value of a `[summary] <key>=<n>` line (-1 if missing).
int64_t SummaryValue(const std::string& key) {
  const std::string s = stats::Summary();
  const std::string needle = "[summary] " + key + "=";
  const auto pos = s.find(needle);
  return pos == std::string::npos ? -1 : std::stoll(s.substr(pos + needle.size()));
}

}  // namespace

TEST_CASE("xenia_cp: MEM_WRITE writes to guest memory", "[xenia_cp]") {
  E();
  Put32(E().data, 0);
  Put32(E().data + 4, 0);
  Emit({MakePacketType3(gpu::kPm4MemWrite, 3), Phys(E().data), 0xF00D, 0xBEEF});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0xF00D);
  CHECK(Get32(E().data + 4) == 0xBEEF);
}

TEST_CASE("xenia_cp: EVENT_WRITE_SHD with bit 31 writes the vblank counter", "[xenia_cp]") {
  E();
  const auto start = Clock::now();
  std::this_thread::sleep_for(120ms);  // a few vblanks
  Put32(E().data, 0xFFFFFFFF);
  Emit({MakePacketType3(gpu::kPm4EventWriteShd, 3), 0x80000007, Phys(E().data), 0x1234});
  Kick();
  REQUIRE(RingDrained(100ms));
  const uint32_t first = Get32(E().data);
  const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  INFO("counter " << first << " after " << elapsed_ms << " ms");
  CHECK(first >= 2);  // not the packet's value 0x1234
  CHECK(first <= uint32_t(elapsed_ms / 16.667) + 2);
  // the counter advances
  std::this_thread::sleep_for(60ms);
  Emit({MakePacketType3(gpu::kPm4EventWriteShd, 3), 0x80000007, Phys(E().data + 4), 0x1234});
  Kick();
  REQUIRE(RingDrained(100ms));
  CHECK(Get32(E().data + 4) > first);
  // without bit 31: the packet's value
  Emit({MakePacketType3(gpu::kPm4EventWriteShd, 3), 0x7, Phys(E().data + 8), 0x1234});
  Kick();
  REQUIRE(RingDrained(100ms));
  CHECK(Get32(E().data + 8) == 0x1234);
}

TEST_CASE("xenia_cp: CP_RB_WPTR in the guest region -> CP_RB_RPTR and write-back within 100 ms", "[xenia_cp]") {
  E();
  Put32(E().wb, 0xFFFFFFFF);
  Emit({MakePacketType2(), MakePacketType2(), MakePacketType3(gpu::kPm4Nop, 2), 0, 0});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(gpu::ReadRegister(gpu::kRegCpRbRptr) == g_wptr);
  // a second time, from where it left off
  Emit({MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0x51});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0x51);
  CHECK(gpu::ReadRegister(gpu::kRegCpRbRptr) == g_wptr);
}

TEST_CASE("xenia_cp: ring buffer wrap-around", "[xenia_cp]") {
  E();
  // fills with NOPs up to the end of the ring and continues from the start
  while (g_wptr != kRingDwords - 1) Emit({MakePacketType2()});
  Kick();
  CHECK(RingDrained(500ms));
  // MEM_WRITE straddling the end: header in the last DWORD
  Emit({MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0x3A9});
  CHECK(g_wptr == 2);
  Kick();
  CHECK(RingDrained(500ms));
  CHECK(Get32(E().data) == 0x3A9);
  CHECK(gpu::ReadRegister(gpu::kRegCpRbRptr) == 2);
}

TEST_CASE("xenia_cp: WAIT_REG_MEM on SCRATCH_REG3 written by the CPU in the region unblocks", "[xenia_cp]") {
  E();
  gpu::WriteRegister(kRegScratchReg3, 0);
  Put32(E().data, 0);
  // wait: (SCRATCH_REG3 & 0xFFFFFFFF) == 0xABCD, then a marker in memory
  Emit({MakePacketType3(gpu::kPm4WaitRegMem, 5), 0x3, kRegScratchReg3, 0xABCD, 0xFFFFFFFF, 0x100,
        MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0x600D});
  Kick();
  std::this_thread::sleep_for(30ms);
  CHECK(Get32(E().data) == 0);  // still waiting
  CHECK(Get32(E().wb) != g_wptr);
  const auto t0 = Clock::now();
  gpu::WriteRegister(kRegScratchReg3, 0xABCD);  // the CPU writes into the guest region
  CHECK(Eventually([] { return Get32(E().data) == 0x600D; }, 100ms));
  const double unblocked_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("unblocked in " << unblocked_ms << " ms");
  CHECK(RingDrained(100ms));
}

TEST_CASE("xenia_cp: WAIT_REG_MEM never satisfied with wait=0x00FFFFFF, gpu::Shutdown returns within 100 ms",
          "[xenia_cp]") {
  E();
  gpu::WriteRegister(kRegScratchReg3, 0);
  Emit({MakePacketType3(gpu::kPm4WaitRegMem, 5), 0x3, kRegScratchReg3, 0xDEAD, 0xFFFFFFFF, 0x00FFFFFF});
  Kick();
  std::this_thread::sleep_for(30ms);
  CHECK(Get32(E().wb) != g_wptr);  // blocked
  const auto t0 = Clock::now();
  gpu::Shutdown();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("Shutdown in " << ms << " ms");
  CHECK(ms < 100.0);
}

// Short wait (wait < 0x100): canary spins without checking the stop; patch 0004 yields the processor and
// checks worker_running_.
TEST_CASE("xenia_cp: short WAIT_REG_MEM never satisfied (wait=0xFF), gpu::Shutdown returns within 100 ms",
          "[xenia_cp]") {
  E();
  gpu::WriteRegister(kRegScratchReg3, 0);
  Emit({MakePacketType3(gpu::kPm4WaitRegMem, 5), 0x3, kRegScratchReg3, 0xDEAD, 0xFFFFFFFF, 0x000000FF});
  Kick();
  std::this_thread::sleep_for(30ms);
  CHECK(Get32(E().wb) != g_wptr);  // blocked
  const auto t0 = Clock::now();
  gpu::Shutdown();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  INFO("Shutdown in " << ms << " ms");
  CHECK(ms < 100.0);
}

TEST_CASE("xenia_cp: PM4_INTERRUPT calls the callback (1, user_data) on a guest thread", "[xenia_cp]") {
  E();
  g_cp_interrupts = 0;
  Call(__imp__VdSetGraphicsInterruptCallback, {kFnInterrupt, 0x77});
  Emit({MakePacketType3(gpu::kPm4Interrupt, 1), 0x2});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Eventually([] { return g_cp_interrupts.load() == 1; }, 100ms));
  CHECK(g_cp_user_data == 0x77);
  CHECK(g_cp_on_guest_thread);
  Call(__imp__VdSetGraphicsInterruptCallback, {0, 0});
}

TEST_CASE("xenia_cp: VdSwap -> IssueSwap(frontbuffer, 1280, 720) once, frames_presented + 1",
          "[xenia_cp]") {
  E();
  const uint32_t p = E().data + 0x100;  // VdSwap parameters
  const uint32_t fetch = p, fb_ptr = p + 0x40, fmt = p + 0x44, cs = p + 0x48, wptr = p + 0x4C, hptr = p + 0x50;
  const uint32_t frontbuffer = 0xBF000000;  // CPU address of the front buffer
  const uint32_t fetch_words[6] = {0x2, (frontbuffer & 0xFFFFF000) | 0x86, (1279) | (719 << 13), 0, 0, 0};
  for (int i = 0; i < 6; ++i) Put32(fetch + i * 4, fetch_words[i]);
  Put32(fb_ptr, frontbuffer);
  Put32(fmt, 0x6);
  Put32(cs, 0);
  Put32(wptr, 1280);
  Put32(hptr, 720);
  const int64_t presented_before = SummaryValue("frames_presented");
  const uint32_t buffer = E().ring + g_wptr * 4;  // 64 DWORDs reserved in the ring
  Call(__imp__VdSwap, {buffer, fetch, 0, 0, 0, fb_ptr, fmt, cs, wptr, hptr});
  g_wptr = (g_wptr + 64) % kRingDwords;
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(SummaryValue("frames_presented") == presented_before + 1);
  const auto swaps = E().cp->Swaps();
  REQUIRE(swaps.size() == 1);
  CHECK(swaps[0][0] == (frontbuffer & 0x1FFFFFFF));
  CHECK(swaps[0][1] == 1280);
  CHECK(swaps[0][2] == 720);
  // the fetch constant written by the type 0 packet is also in the guest region
  CHECK(gpu::ReadRegister(gpu::kRegShaderConstantFetch00 + 1) == ((frontbuffer & 0x1FFFF000) | 0x86));
}

TEST_CASE("xenia_cp: SCRATCH_REG0 written by the CP in the guest region and in memory with SCRATCH_UMSK=1",
          "[xenia_cp]") {
  E();
  const uint32_t area = E().data + 0x200;
  for (int i = 0; i < 8; ++i) Put32(area + i * 4, 0xCCCCCCCC);
  Emit({MakePacketType0(gpu::kRegScratchUmsk, 2), 0x1, area & 0x1FFFFFFF,  // UMSK = bit 0, ADDR
        MakePacketType0(gpu::kRegScratchReg0, 2), 0x111, 0x222});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(area + 0) == 0x111);
  CHECK(Get32(area + 4) == 0xCCCCCCCC);  // bit 1 not enabled
  CHECK(gpu::ReadRegister(gpu::kRegScratchReg0) == 0x111);
  CHECK(gpu::ReadRegister(gpu::kRegScratchReg0 + 1) == 0x222);
  CHECK(gpu::ReadRegister(gpu::kRegScratchUmsk) == 0x1);
  // COHER_STATUS_HOST written by the CP: in the region with bit 31 (final value)
  Emit({MakePacketType0(gpu::kRegCoherStatusHost, 1), 0x03000000});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(gpu::ReadRegister(gpu::kRegCoherStatusHost) == 0x83000000);
}

TEST_CASE("xenia_cp: WAIT_REG_MEM on COHER_STATUS_HOST: MakeCoherent clears it also in the guest region",
          "[xenia_cp]") {
  E();
  Put32(E().data, 0);
  // the CP writes COHER_STATUS_HOST (bit 31 = pending request), then waits
  // for bit 31 to clear (MakeCoherent) and writes a marker
  Emit({MakePacketType0(gpu::kRegCoherStatusHost, 1), 0x03000000,
        MakePacketType3(gpu::kPm4WaitRegMem, 5), 0x3, gpu::kRegCoherStatusHost, 0, 0x80000000, 0x100,
        MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0xC0E7});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0xC0E7);
  // the CPU reads the guest region: no bit 31 left hanging
  CHECK(gpu::ReadRegister(gpu::kRegCoherStatusHost) == 0);
}

TEST_CASE("xenia_cp: DRAW_INDX with a failing IssueDraw -> gpu_draws=1, gpu_draws_skipped=1", "[xenia_cp]") {
  E();
  E().cp->draw_result = false;
  // viz query 0; VGT_DRAW_INITIATOR: triangle list (4), auto-index (2 << 6), 3 indici
  Emit({MakePacketType3(gpu::kPm4DrawIndx, 2), 0, 4u | (2u << 6) | (3u << 16)});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(E().cp->draws.load() == 1);
  CHECK(SummaryValue("gpu_draws") == 1);
  CHECK(SummaryValue("gpu_draws_skipped") == 1);
  // a successful draw counts only in gpu_draws
  E().cp->draw_result = true;
  Emit({MakePacketType3(gpu::kPm4DrawIndx, 2), 0, 4u | (2u << 6) | (3u << 16)});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(SummaryValue("gpu_draws") == 2);
  CHECK(SummaryValue("gpu_draws_skipped") == 1);
}

TEST_CASE("xenia_cp: opcode 0x7E -> gpu_unknown_packet 0x7E=1, rest of the buffer discarded and in the log",
          "[xenia_cp]") {
  E();
  gow2_test::TempDir dir("gow2_xenia_cp_log");
  std::filesystem::create_directories(dir.path);
  LogInit(dir.path / "gow2.log");
  Put32(E().data, 0);
  // As in Xenia the rest of the block after an unknown opcode is discarded
  // (ExecutePacketType3 returns false): the MEM_WRITE after it is not executed.
  Emit({MakePacketType3(0x7E, 2), 0x1, 0x2, MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0xDEAD});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0);
  CHECK(stats::Summary().find("[summary] gpu_unknown_packet 0x7E=1\n") != std::string::npos);
  CHECK(gpu::GetCounters().unknown_packets == 1);
  LogInit({});
  std::ifstream log(dir.path / "gow2.log");
  const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
  CHECK(text.find("[gpu] PM4 packet not executed: opcode 0x7E (rest of buffer discarded)\n") != std::string::npos);
  // the command processor continues with the next block
  Emit({MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0x7E7E});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0x7E7E);
}

TEST_CASE("xenia_cp: Pause suspends the CP thread, Resume resumes it", "[xenia_cp]") {
  E();
  Put32(E().data, 0);
  E().cp->Pause();  // returns when the CP thread has suspended
  Emit({MakePacketType3(gpu::kPm4MemWrite, 2), Phys(E().data), 0x9A});
  Kick();
  std::this_thread::sleep_for(50ms);
  CHECK(Get32(E().data) == 0);
  E().cp->Resume();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0x9A);
}
