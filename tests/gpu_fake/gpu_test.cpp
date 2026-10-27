// Fake GPU (backend runtime/gpu_fake/gpu.* on the common module runtime/gpu:
// gpu_system.*, vd_exports.cpp): ring buffer consumed
// by the command processor thread after CP_RB_WPTR is written, write-back
// read pointer, 60 Hz vblank callback on a guest thread,
// interrupts from PM4_INTERRUPT, VdSwap and the other Vd* functions. Real GuestMemory arena,
// "guest functions" registered in a fake function table; no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "gpu/gpu_system.h"
#include "gpu_fake/gpu.h"
#include "gpu_fake/pm4.h"
#include "guest_memory.h"
#include "imports.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "kernel/threads.h"

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSetGraphicsInterruptCallback);
PPC_EXTERN_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress);
PPC_EXTERN_FUNC(__imp__VdGetSystemCommandBuffer);
PPC_EXTERN_FUNC(__imp__VdSwap);
PPC_EXTERN_FUNC(__imp__VdQueryVideoMode);
PPC_EXTERN_FUNC(__imp__VdQueryVideoFlags);
PPC_EXTERN_FUNC(__imp__VdGetCurrentDisplayInformation);
PPC_EXTERN_FUNC(__imp__VdGetCurrentDisplayGamma);
PPC_EXTERN_FUNC(__imp__VdInitializeEngines);
PPC_EXTERN_FUNC(__imp__VdShutdownEngines);
PPC_EXTERN_FUNC(__imp__VdIsHSIOTrainingSucceeded);
PPC_EXTERN_FUNC(__imp__VdPersistDisplay);
PPC_EXTERN_FUNC(__imp__VdRetrainEDRAM);
PPC_EXTERN_FUNC(__imp__VdRetrainEDRAMWorker);
PPC_EXTERN_FUNC(__imp__VdSetDisplayMode);
PPC_EXTERN_FUNC(__imp__VdInitializeScalerCommandBuffer);
PPC_EXTERN_FUNC(__imp__VdCallGraphicsNotificationRoutines);
PPC_EXTERN_FUNC(__imp__VdEnableDisableClockGating);

namespace {

using namespace kernel;
using namespace gpu_fake;  // pm4.h: also the gpu:: names
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }
uint16_t Get16(uint32_t a) { return *GuestPtr<be<uint16_t>>(a); }
float GetF(uint32_t a) { return std::bit_cast<float>(Get32(a)); }

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
std::atomic<int> g_vblanks{0}, g_cp_interrupts{0};
std::atomic<uint32_t> g_user_data{0}, g_bad_source{0};
std::atomic<bool> g_on_guest_thread{true};
std::atomic<uint32_t> g_vblank_cpu{0xFF};

PPC_FUNC(FnInterrupt) {
  if (!GuestThread::Current()) g_on_guest_thread = false;
  g_user_data = ctx.r4.u32;
  if (ctx.r3.u32 == 0) {
    g_vblank_cpu = *GuestPtr<uint8_t>(ctx.r13.u32 + kPcrCurrentCpu);
    ++g_vblanks;
  } else if (ctx.r3.u32 == 1) {
    ++g_cp_interrupts;
  } else {
    g_bad_source = ctx.r3.u32;
  }
}

// Slow callback: detects overlaps between interrupts and records PCR[0] (TLS).
constexpr uint32_t kFnSlowInterrupt = 0x82000200;
std::atomic<int> g_inside{0}, g_slow_calls[2];
std::atomic<bool> g_overlap{false}, g_tls_nonzero_inside{false};
std::atomic<uint32_t> g_pcr[2];
PPC_FUNC(FnSlowInterrupt) {
  if (g_inside.fetch_add(1) != 0) g_overlap = true;
  const uint32_t src = ctx.r3.u32 & 1;
  g_pcr[src] = ctx.r13.u32;
  if (*GuestPtr<be<uint32_t>>(ctx.r13.u32 + kPcrTlsPtr) != 0u) g_tls_nonzero_inside = true;
  std::this_thread::sleep_for(3ms);
  ++g_slow_calls[src];
  g_inside.fetch_sub(1);
}

PPCFunc* Resolve(uint32_t guest) {
  if (guest == kFnInterrupt) return &FnInterrupt;
  if (guest == kFnSlowInterrupt) return &FnSlowInterrupt;
  return nullptr;
}

struct Env {
  GuestMemory mem;
  uint32_t ring = 0, ring_phys = 0, wb = 0, wb_phys = 0, data = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(InitThreads(mem, &Resolve, 0x40000));
    REQUIRE(gpu::Init(mem, gpu_fake::CreateBackend()));
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

void Kick() { Put32(kRegisterBase + kRegCpRbWptr * 4, g_wptr); }

bool RingDrained(std::chrono::milliseconds limit) {
  return Eventually([] { return Get32(E().wb) == g_wptr; }, limit);
}

uint64_t FramesPresented() {
  const std::string s = stats::Summary();
  const auto pos = s.find("frames_presented=");
  return pos == std::string::npos ? 0 : std::stoull(s.substr(pos + 17));
}

}  // namespace

TEST_CASE("gpu: register region committed, fixed Xenia values", "[gpu]") {
  E();
  CHECK(ReadRegister(0x0F00) == 0x08100748);  // RB_EDRAM_TIMING
  CHECK(ReadRegister(0x1961) == 0x050002D0);  // 1280x720
  CHECK(Get32(kRegisterBase + 0x1951 * 4) == 1);
  WriteRegister(0x4FFF, 0xCAFE);
  CHECK(Get32(kRegisterBase + 0x4FFF * 4) == 0xCAFE);
  // the whole region is accessible
  Put32(kRegisterBase + kRegisterRegionSize - 4, 0x1);
  CHECK(Get32(kRegisterBase + kRegisterRegionSize - 4) == 0x1);
  WriteRegister(kRegisterCount, 1);  // outside the register file: ignored
  CHECK(ReadRegister(kRegisterCount) == 0);
}

TEST_CASE("gpu: writing CP_RB_WPTR -> write-back read pointer = write pointer within 100 ms", "[gpu]") {
  E();
  Put32(E().wb, 0xFFFFFFFF);
  Emit({MakePacketType3(kPm4MemWrite, 2), (E().data & 0x1FFFFFFF) | 2, 0xF00D, MakePacketType2(),
        MakePacketType3(kPm4EventWriteShd, 3), 0x7, ((E().data + 4) & 0x1FFFFFFF) | 2, 0x1234});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0xF00D);
  CHECK(Get32(E().data + 4) == 0x1234);
  CHECK(ReadRegister(kRegCpRbRptr) == g_wptr);
  // more packets, with an opcode that is not executed
  const uint64_t unknown = GetCounters().unknown_packets;
  Emit({MakePacketType3(kPm4DrawIndx, 2), 0, 0, MakePacketType3(kPm4MemWrite, 2), (E().data & 0x1FFFFFFF) | 2, 0xBEEF});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(E().data) == 0xBEEF);
  CHECK(GetCounters().unknown_packets == unknown + 1);
}

TEST_CASE("gpu: ring buffer wrap-around", "[gpu]") {
  E();
  // fills up to the end of the ring with NOPs and continues from the start
  while (g_wptr != kRingDwords - 1) Emit({MakePacketType2()});
  Kick();
  CHECK(RingDrained(500ms));
  // MEM_WRITE straddling the end: header in the last DWORD
  Emit({MakePacketType3(kPm4MemWrite, 2), (E().data & 0x1FFFFFFF) | 2, 0x3A9});
  CHECK(g_wptr == 2);
  Kick();
  CHECK(RingDrained(500ms));
  CHECK(Get32(E().data) == 0x3A9);
}

TEST_CASE("gpu: vblank callback at ~60 Hz on a guest thread, CPU 2", "[gpu]") {
  E();
  g_vblanks = 0;
  const auto start = Clock::now();
  Call(__imp__VdSetGraphicsInterruptCallback, {kFnInterrupt, 0x4004B780});
  std::this_thread::sleep_for(1s);
  Call(__imp__VdSetGraphicsInterruptCallback, {0, 0});
  const int n = g_vblanks.load();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  INFO("vblank in " << elapsed_ms << " ms: " << n);
  // Under load (ctest -j) the thread may lose periods (it restarts from
  // now, without bursts): wide lower bound; never faster than 60 Hz.
  CHECK(n >= 30);
  CHECK(n <= int(elapsed_ms / 16.667) + 2);
  CHECK(g_user_data == 0x4004B780);
  CHECK(g_on_guest_thread);
  CHECK(g_vblank_cpu == 2);
  CHECK(g_bad_source == 0);
  // callback removed: no further calls
  std::this_thread::sleep_for(50ms);
  const int after = g_vblanks.load();
  std::this_thread::sleep_for(100ms);
  CHECK(g_vblanks.load() == after);
}

TEST_CASE("gpu: PM4_INTERRUPT calls the callback with source 1 on the command processor thread", "[gpu]") {
  E();
  g_cp_interrupts = 0;
  Call(__imp__VdSetGraphicsInterruptCallback, {kFnInterrupt, 0x77});
  Emit({MakePacketType3(kPm4Interrupt, 1), 0x2});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Eventually([] { return g_cp_interrupts.load() == 1; }, 100ms));
  CHECK(g_on_guest_thread);
  Call(__imp__VdSetGraphicsInterruptCallback, {0, 0});
}

TEST_CASE("gpu: vblank and command processor interrupts never overlap, PCR TLS is 0 during the callback", "[gpu]") {
  E();
  Call(__imp__VdSetGraphicsInterruptCallback, {kFnSlowInterrupt, 0x9});
  // one INTERRUPT at a time (awaited), at intervals that are not multiples of the vblank:
  // the vblank callbacks (slow, 3 ms) often straddle them
  for (int i = 0; i < 40; ++i) {
    const int before = g_slow_calls[1].load();
    Emit({MakePacketType3(kPm4Interrupt, 1), 0x1});
    Kick();
    REQUIRE(Eventually([&] { return g_slow_calls[1].load() > before; }, 500ms));
    std::this_thread::sleep_for(7ms);
  }
  CHECK(RingDrained(500ms));
  Call(__imp__VdSetGraphicsInterruptCallback, {0, 0});
  CHECK(Eventually([] { return g_inside.load() == 0; }, 200ms));
  INFO("vblank " << g_slow_calls[0].load() << ", CP " << g_slow_calls[1].load());
  CHECK(g_slow_calls[0].load() >= 10);  // ~0.4 s of vblank
  CHECK(g_slow_calls[1].load() == 40);
  CHECK_FALSE(g_overlap.load());
  CHECK_FALSE(g_tls_nonzero_inside.load());
  // after the callback the PCR's TLS pointer is restored (TLS block at PCR + kTlsOffset)
  // (the restore follows the callback's exit: g_inside at 0 is not enough)
  for (int src = 0; src < 2; ++src) {
    const uint32_t pcr = g_pcr[src].load();
    REQUIRE(pcr != 0);
    CHECK(Eventually([&] { return Get32(pcr + kPcrTlsPtr) == pcr + kTlsOffset; }, 500ms));
  }
  CHECK(g_pcr[0].load() != g_pcr[1].load());  // two different threads
}

TEST_CASE("gpu: SCRATCH_REGs written by the command processor end up in memory (SCRATCH_UMSK/ADDR)", "[gpu]") {
  E();
  const uint32_t area = E().data + 0x200;
  for (int i = 0; i < 8; ++i) Put32(area + i * 4, 0xCCCCCCCC);
  Emit({MakePacketType0(kRegScratchUmsk, 2), 0x05, area & 0x1FFFFFFF,  // UMSK = bits 0 and 2, ADDR
        MakePacketType0(kRegScratchReg0, 3), 0x111, 0x222, 0x333});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(Get32(area + 0) == 0x111);
  CHECK(Get32(area + 4) == 0xCCCCCCCC);  // bit 1 not enabled
  CHECK(Get32(area + 8) == 0x333);
  CHECK(ReadRegister(kRegScratchReg0 + 1) == 0x222);
  // a CPU write (memory) has no side effects
  WriteRegister(kRegScratchReg0, 0x444);
  CHECK(Get32(area) == 0x111);
}

TEST_CASE("gpu: COHER_STATUS_HOST written by the command processor gets bit 31, WAIT_REG_MEM clears it", "[gpu]") {
  E();
  Emit({MakePacketType0(kRegCoherStatusHost, 1), 0x03000000});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(ReadRegister(kRegCoherStatusHost) == 0x83000000);
  Emit({MakePacketType3(kPm4WaitRegMem, 5), 0x3, kRegCoherStatusHost, 0, 0x80000000, 0x100,
        MakePacketType3(kPm4MemWrite, 2), (E().data & 0x1FFFFFFF) | 2, 0xC0E});
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(ReadRegister(kRegCoherStatusHost) == 0);
  CHECK(Get32(E().data) == 0xC0E);
}

TEST_CASE("gpu: VdSwap writes fetch and XE_SWAP into the ring, the frame is counted", "[gpu]") {
  E();
  const uint32_t p = E().data + 0x100;  // VdSwap parameters
  const uint32_t fetch = p, fb_ptr = p + 0x40, fmt = p + 0x44, cs = p + 0x48, wptr = p + 0x4C, hptr = p + 0x50;
  const uint32_t frontbuffer = 0xBF000000;  // CPU address of the front buffer
  // fetch constant of the front buffer (Xenia xe_gpu_texture_fetch_t): base in dword_1 >> 12
  const uint32_t fetch_words[6] = {0x2, (frontbuffer & 0xFFFFF000) | 0x86, (1279) | (719 << 13), 0, 0, 0};
  for (int i = 0; i < 6; ++i) Put32(fetch + i * 4, fetch_words[i]);
  Put32(fb_ptr, frontbuffer);
  Put32(fmt, 0x6);  // k_8_8_8_8
  Put32(cs, 0);
  Put32(wptr, 1280);
  Put32(hptr, 720);

  const uint64_t frames_before = GetCounters().frames;
  const uint64_t presented_before = FramesPresented();
  // the game reserves 64 DWORDs in the ring and has VdSwap fill them
  if (g_wptr + 64 > kRingDwords) {
    while (g_wptr != 0) Emit({MakePacketType2()});
  }
  const uint32_t buffer = E().ring + g_wptr * 4;
  Call(__imp__VdSwap, {buffer, fetch, 0, 0, 0, fb_ptr, fmt, cs, wptr, hptr});
  CHECK(FramesPresented() == presented_before + 1);
  CHECK(Get32(buffer) == MakePacketType0(kRegShaderConstantFetch00, 6));
  CHECK(Get32(buffer + 4) == 0x2);
  CHECK(Get32(buffer + 8) == ((frontbuffer & 0x1FFFF000) | 0x86));  // physical address
  CHECK(Get32(buffer + 28) == MakePacketType3(kPm4XeSwap, 4));
  CHECK(Get32(buffer + 32) == kSwapSignature);
  CHECK(Get32(buffer + 36) == (frontbuffer & 0x1FFFFFFF));
  CHECK(Get32(buffer + 40) == 1280);
  CHECK(Get32(buffer + 44) == 720);
  for (int i = 12; i < 64; ++i) CHECK(Get32(buffer + i * 4) == MakePacketType2());
  g_wptr = (g_wptr + 64) % kRingDwords;
  Kick();
  CHECK(RingDrained(100ms));
  CHECK(GetCounters().frames == frames_before + 1);
  CHECK(ReadRegister(kRegShaderConstantFetch00 + 1) == ((frontbuffer & 0x1FFFF000) | 0x86));
}

TEST_CASE("gpu: video mode 1280x720 60 Hz widescreen and other Vd* functions", "[gpu]") {
  E();
  const uint32_t m = E().data + 0x400;
  std::memset(GuestPtr<uint8_t>(m), 0xCC, 0x200);
  Call(__imp__VdQueryVideoMode, {m});
  CHECK(Get32(m + 0x00) == 1280);
  CHECK(Get32(m + 0x04) == 720);
  CHECK(Get32(m + 0x08) == 0);  // interlaced
  CHECK(Get32(m + 0x0C) == 1);  // widescreen
  CHECK(Get32(m + 0x10) == 1);  // HD
  CHECK(GetF(m + 0x14) == 60.0f);
  CHECK(Get32(m + 0x18) == 1);  // NTSC
  CHECK(Get32(m + 0x1C) == 0x4A);
  CHECK(Get32(m + 0x20) == 0x01);
  CHECK(Get32(m + 0x24) == 0);
  CHECK(Get32(m + 0x2C) == 0);
  CHECK(Get32(m + 0x30) == 0xCCCCCCCC);  // 48 bytes, no more
  CHECK(Call(__imp__VdQueryVideoFlags, {}) == 3);

  std::memset(GuestPtr<uint8_t>(m), 0xCC, 0x200);
  Call(__imp__VdGetCurrentDisplayInformation, {m});
  CHECK(Get16(m + 0x00) == 1280);
  CHECK(Get16(m + 0x02) == 720);
  CHECK(Get32(m + 0x08 + 0x08) == 1280);  // scaler_source_rect.x2
  CHECK(Get32(m + 0x08 + 0x0C) == 720);
  CHECK(Get32(m + 0x08 + 0x10) == 1280);  // scaled_output
  CHECK(Get32(m + 0x08 + 0x14) == 720);
  CHECK(Get32(m + 0x08 + 0x18) == 1);  // vertical_filter_type
  CHECK(Get32(m + 0x08 + 0x28) == 1);  // horizontal_filter_type
  CHECK(Get16(m + 0x40) == 320);
  CHECK(Get16(m + 0x42) == 180);
  CHECK(Get16(m + 0x48) == 1280);
  CHECK(Get16(m + 0x4A) == 720);
  CHECK(GetF(m + 0x4C) == 60.0f);
  CHECK(Get32(m + 0x50) == 0);
  CHECK(Get16(m + 0x56) == 1280);
  CHECK(Get32(m + 0x58) == 0xCCCCCCCC);

  Call(__imp__VdGetCurrentDisplayGamma, {m, m + 4});
  CHECK(Get32(m) == 2);  // BT.709
  CHECK(GetF(m + 4) == 2.22222233f);

  CHECK(Call(__imp__VdInitializeEngines, {0x1DDD0001, 0x82990AC0, 0, 0x82088110, 0x82088590}) == 1);
  Call(__imp__VdShutdownEngines, {});
  CHECK(Call(__imp__VdIsHSIOTrainingSucceeded, {}) == 1);
  CHECK(Call(__imp__VdSetDisplayMode, {0x40000000}) == 0);
  CHECK(Call(__imp__VdRetrainEDRAM, {0, 0, 0, 0, 0, 0}) == 0);
  CHECK(Call(__imp__VdRetrainEDRAMWorker, {0}) == 0);
  CHECK(Call(__imp__VdEnableDisableClockGating, {1}) == 0);
  CHECK(Call(__imp__VdCallGraphicsNotificationRoutines, {1, m}) == 0);
  Call(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, {0xBFFF5008});

  // VdPersistDisplay: physical block that can be freed with MmFreePhysicalMemory
  Put32(m, 0);
  CHECK(Call(__imp__VdPersistDisplay, {0, m}) == 1);
  const uint32_t persisted = Get32(m);
  CHECK(Memory()->phys.Contains(persisted));
  CHECK(Memory()->phys.Size(persisted) != 0);
  Memory()->phys.Free(persisted);
  CHECK(Call(__imp__VdPersistDisplay, {0, 0}) == 1);

  // VdGetSystemCommandBuffer: 0x94 zeroed bytes, Xenia markers
  std::memset(GuestPtr<uint8_t>(m), 0xCC, 0x200);
  Call(__imp__VdGetSystemCommandBuffer, {m, m + 0x100});
  CHECK(Get32(m) == 0xBEEF0000);
  CHECK(Get32(m + 0x90) == 0);
  CHECK(Get32(m + 0x94) == 0xCCCCCCCC);
  CHECK(Get32(m + 0x100) == 0xBEEF0001);

  // VdInitializeScalerCommandBuffer: dest (arg 10, on the stack) filled with NOPs
  std::memset(GuestPtr<uint8_t>(m), 0xCC, 0x200);
  CHECK(Call(__imp__VdInitializeScalerCommandBuffer, {0, 0x02D00500, 0, 0x02D00500, 0x02D00500, 7, 0, 7, 0, 0, m, 5}) ==
        5);
  for (int i = 0; i < 5; ++i) CHECK(Get32(m + i * 4) == 0x80000000);
  CHECK(Get32(m + 20) == 0xCCCCCCCC);
  // dest_count that exceeds the end of the guest space: rejected, nothing written
  CHECK(Call(__imp__VdInitializeScalerCommandBuffer,
             {0, 0x02D00500, 0, 0x02D00500, 0x02D00500, 7, 0, 7, 0, 0, 0xFFFFFFF0, 0x100}) == 0);
}

TEST_CASE("gpu: Vd* variables initialized like Xenia (device, clock, VdHSIOCalibrationLock)", "[gpu]") {
  E();
  // slots and areas zeroed like ResolveImports; areas dirtied to see the writes
  const uint32_t slots = E().mem.Alloc(0x20, 4);
  uint32_t areas[5];
  for (int i = 0; i < 5; ++i) {
    areas[i] = E().mem.Alloc(0x1000, 0x10);
    std::memset(GuestPtr<uint8_t>(areas[i]), 0xCC, 0x40);
    Put32(slots + i * 4, areas[i]);
  }
  const std::vector<ImportRecord> records = {
      {"xboxkrnl.exe", slots + 0, 0x1BE, 0},   // VdGlobalDevice
      {"xboxkrnl.exe", slots + 4, 0x1BF, 0},   // VdGlobalXamDevice
      {"xboxkrnl.exe", slots + 8, 0x1C0, 0},   // VdGpuClockInMHz
      {"xboxkrnl.exe", slots + 12, 0x1C1, 0},  // VdHSIOCalibrationLock
      {"xboxkrnl.exe", slots + 16, 0x1C1, 1},  // thunk (type 1): ignored
  };
  InitVideoVariables(records);
  CHECK(Get32(areas[0]) == 0);
  CHECK(Get32(areas[1]) == 0);
  CHECK(Get32(areas[2]) == 500);
  CHECK(Get32(areas[3] + 16) == 0xFFFFFFFF);  // lock_count: free critical section
  CHECK(Get32(areas[3] + 24) == 0);           // owning_thread
  CHECK(Get32(areas[4]) == 0xCCCCCCCC);
  CHECK(CriticalSectionTryEnter(areas[3]));
  CriticalSectionLeave(areas[3]);
}

// Last: stops the threads (even with a never-satisfied WAIT_REG_MEM in progress).
TEST_CASE("gpu: Shutdown stops the command processor and vblank", "[gpu]") {
  E();
  g_vblanks = 0;
  Call(__imp__VdSetGraphicsInterruptCallback, {kFnInterrupt, 0x1});
  CHECK(Eventually([] { return g_vblanks.load() > 0; }, 500ms));
  Emit({MakePacketType3(kPm4WaitRegMem, 5), 0x0 /* never */, 0x578, 0, 0, 0x100});
  Kick();
  std::this_thread::sleep_for(20ms);
  const auto start = Clock::now();
  gpu::Shutdown();
  CHECK(Clock::now() - start < 1s);
  const int n = g_vblanks.load();
  std::this_thread::sleep_for(100ms);
  CHECK(g_vblanks.load() == n);
  gpu::Shutdown();  // idempotent
}
