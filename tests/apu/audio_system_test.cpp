// Audio system (runtime/apu/audio_system.*): XAudio driver clients,
// "Audio Worker" thread paced by the null sink's credits, frame
// conversion, WAV capture, XAudio* exports. Real GuestMemory arena and test
// kernel; the "game" callback is a host function at 0x82000100. Synthetic
// data only.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "apu/audio_system.h"
#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/threads.h"
#include "log.h"
#include "temp_dir.h"

PPC_EXTERN_FUNC(__imp__XAudioRegisterRenderDriverClient);
PPC_EXTERN_FUNC(__imp__XAudioUnregisterRenderDriverClient);
PPC_EXTERN_FUNC(__imp__XAudioSubmitRenderDriverFrame);
PPC_EXTERN_FUNC(__imp__XAudioGetVoiceCategoryVolume);
PPC_EXTERN_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask);

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;


constexpr uint32_t kFnCallback = 0x82000100;

// Behaviour of the test callback, configured by the test.
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_last_r3{0};
std::atomic<bool> g_on_guest_thread{false};
std::atomic<bool> g_submit{false};
std::atomic<uint32_t> g_submit_handle{0};
std::atomic<uint32_t> g_samples{0};
std::atomic<uint32_t> g_sleep_ms{0};
std::atomic<uint32_t> g_unregister_self{0};  // handle to unregister from the callback (0 = no)
std::atomic<uint32_t> g_unregister_result{0xFFFFFFFF};
std::atomic<uint32_t> g_entries{0}, g_exits{0};  // callback entries/exits
std::atomic<bool> g_inside{false};
// Argument kHoldArg: the callback stays inside while g_hold is true.
constexpr uint32_t kHoldArg = 0xB10CB10C;
std::atomic<bool> g_hold{false};
std::atomic<bool> g_hold_entered{false};

uint32_t Arg(uint32_t wrapped) { return *kernel::GuestPtr<be<uint32_t>>(wrapped); }

PPC_FUNC(TestCallback) {
  ++g_entries;
  g_inside = true;
  ++g_calls;
  g_last_r3 = ctx.r3.u32;
  if (Arg(ctx.r3.u32) == kHoldArg) {
    g_hold_entered = true;
    while (g_hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_inside = false;
    ++g_exits;
    return;  // leaves immediately, without the rest of the configured behaviour
  }
  g_on_guest_thread = kernel::GuestThread::Current() != nullptr;
  if (g_submit) apu::SubmitFrame(g_submit_handle, g_samples);
  if (const uint32_t ms = g_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  if (const uint32_t h = g_unregister_self.exchange(0)) g_unregister_result = apu::UnregisterClient(h);
  g_inside = false;
  ++g_exits;
}

PPCFunc* Resolve(uint32_t guest) { return guest == kFnCallback ? &TestCallback : nullptr; }

struct Env {
  GuestMemory mem;
  Env() {
    REQUIRE(mem.Init());
    kernel::g_base = mem.base;
    REQUIRE(kernel::InitMemory(mem));
    REQUIRE(kernel::InitThreads(mem, &Resolve, 0x40000));
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = kernel::Memory()->heap.Alloc(size);
    REQUIRE(a != 0);
    std::memset(mem.base + a, 0, size);
    return a;
  }
};
Env& E() {
  static Env env;
  return env;
}

uint32_t Get32(uint32_t a) { return *kernel::GuestPtr<be<uint32_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *kernel::GuestPtr<be<uint32_t>>(a) = v; }

std::unique_ptr<plat::AudioOut> NullFactory(plat::AudioSinkKind, plat::AudioOut::BufferDone done) {
  return plat::CreateNullAudioOut(std::move(done));
}

// Factory that keeps a copy of every BufferDone handed out (even after the
// sink is destroyed), to simulate a late BufferDone.
std::vector<plat::AudioOut::BufferDone> g_saved_done;
std::unique_ptr<plat::AudioOut> SavingFactory(plat::AudioSinkKind, plat::AudioOut::BufferDone done) {
  g_saved_done.push_back(done);
  return plat::CreateNullAudioOut(std::move(done));
}

// Value of a counter from the summary (`[summary] <name>=<n>`).
uint64_t Counter(const std::string& name) {
  const std::string s = kernel::stats::Summary();
  const std::string key = "[summary] " + name + "=";
  const size_t pos = s.find(key);
  REQUIRE(pos != std::string::npos);
  return std::stoull(s.substr(pos + key.size()));
}

void ResetCallback() {
  g_submit = false;
  g_sleep_ms = 0;
  g_unregister_self = 0;
  g_unregister_result = 0xFFFFFFFF;
}

template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
  const auto end = Clock::now() + timeout;
  while (!pred()) {
    if (Clock::now() >= end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// callback_ptr = {kFnCallback, arg}; returns the handle (0 on failure).
uint32_t Register(Env& e, uint32_t arg, uint32_t* status = nullptr) {
  const uint32_t cb = e.Alloc(8), drv = e.Alloc(4);
  Put32(cb, kFnCallback);
  Put32(cb + 4, arg);
  const uint32_t st = apu::RegisterClient(cb, drv);
  if (status) *status = st;
  return st == 0 ? Get32(drv) : 0;
}

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[2] = {&ctx.r3.u64, &ctx.r4.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, kernel::g_base);
  return ctx.r3.u32;
}

struct ApuSession {
  explicit ApuSession(apu::AudioOptions options = {}) {
    options.sink = plat::AudioSinkKind::kNull;
    REQUIRE(apu::Init(options, &NullFactory));
  }
  ~ApuSession() { apu::Shutdown(); }
};

}  // namespace

TEST_CASE("audio_system: ConvertFrame from planar BE to interleaved LE") {
  std::vector<uint8_t> in(plat::kAudioFrameFloats * 4);
  for (uint32_t c = 0; c < 6; ++c)
    for (uint32_t i = 0; i < 256; ++i) {
      const float v = float(c * 1000 + i);
      const uint32_t be_bits = std::byteswap(std::bit_cast<uint32_t>(v));
      std::memcpy(&in[(c * 256 + i) * 4], &be_bits, 4);
    }
  std::array<float, plat::kAudioFrameFloats> out{};
  apu::ConvertFrame(in.data(), out);
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < 256; ++i)
    for (uint32_t c = 0; c < 6; ++c) wrong += out[i * 6 + c] != float(c * 1000 + i);
  CHECK(wrong == 0);
  CHECK(out[1 * 6 + 2] == 2001.0f);
}

TEST_CASE("audio_system: ParseAudioSink accepts only lowercase xaudio2 and null") {
  CHECK(apu::ParseAudioSink("null") == plat::AudioSinkKind::kNull);
  CHECK(apu::ParseAudioSink("xaudio2") == plat::AudioSinkKind::kXAudio2);
  CHECK_FALSE(apu::ParseAudioSink("NULL").has_value());
  CHECK_FALSE(apu::ParseAudioSink("").has_value());
  CHECK_FALSE(apu::ParseAudioSink("wasapi").has_value());
}

TEST_CASE("audio_system: the callback receives the wrapped argument in r3, on a guest thread") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  g_last_r3 = 0;
  g_on_guest_thread = false;
  const uint32_t before = g_calls;
  const uint32_t handle = Register(e, 0xCAFEBABE);
  CHECK(handle == 0x41550000);
  REQUIRE(WaitFor([&] { return g_calls.load() > before; }, 1000ms));
  const uint32_t p = g_last_r3;
  REQUIRE(p != 0);
  CHECK(Get32(p) == 0xCAFEBABE);
  CHECK(g_on_guest_thread.load());
  CHECK(apu::UnregisterClient(handle) == 0);
}

TEST_CASE("audio_system: callback that submits every time, null sink pacing") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  const uint64_t cb0 = Counter("audio_callbacks"), fr0 = Counter("audio_frames_submitted");
  g_samples = e.Alloc(plat::kAudioFrameFloats * 4);
  g_submit_handle = 0x41550000;
  g_submit = true;
  const uint32_t handle = Register(e, 1);
  REQUIRE(handle == 0x41550000);
  std::this_thread::sleep_for(2000ms);
  CHECK(apu::UnregisterClient(handle) == 0);
  g_submit = false;
  const uint64_t callbacks = Counter("audio_callbacks") - cb0;
  const uint64_t frames = Counter("audio_frames_submitted") - fr0;
  INFO("callbacks " << callbacks << ", frames " << frames);
  CHECK(callbacks >= 16 + 338);
  CHECK(frames + 1 >= callbacks);
  CHECK(frames <= callbacks + 1);
}

TEST_CASE("audio_system: callback that does not submit stops at 16 credits (85 ms of queued audio)") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  const uint64_t cb0 = Counter("audio_callbacks");
  const uint32_t handle = Register(e, 2);
  REQUIRE(handle != 0);
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb0 >= 16; }, 1000ms));
  std::this_thread::sleep_for(50ms);
  CHECK(Counter("audio_callbacks") - cb0 == 16);
  std::this_thread::sleep_for(200ms);
  CHECK(Counter("audio_callbacks") - cb0 == 16);
  CHECK(apu::UnregisterClient(handle) == 0);
}

// Review Focus 1: the game unregisters and registers again (sub_82956CD0).
TEST_CASE("audio_system: Unregister during callbacks, then a new registration in the same slot") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  g_samples = e.Alloc(plat::kAudioFrameFloats * 4);
  g_submit_handle = 0x41550000;
  g_submit = true;
  const uint32_t handle = Register(e, 3);
  REQUIRE(handle == 0x41550000);
  const uint32_t start = g_calls;
  REQUIRE(WaitFor([&] { return g_calls.load() > start + 100; }, 2000ms));
  CHECK(apu::UnregisterClient(handle) == 0);
  const uint32_t after = g_calls;
  std::this_thread::sleep_for(100ms);
  CHECK(g_calls.load() == after);

  // Same slot, 16 new credits: without submissions the callbacks stop at 16.
  g_submit = false;
  const uint64_t cb0 = Counter("audio_callbacks");
  const uint32_t again = Register(e, 4);
  CHECK(again == 0x41550000);
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb0 >= 1; }, 1000ms));
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb0 >= 16; }, 1000ms));
  std::this_thread::sleep_for(150ms);  // the old sink's BufferDone calls give no credits
  CHECK(Counter("audio_callbacks") - cb0 == 16);
  CHECK(Get32(g_last_r3) == 4);
  CHECK(apu::UnregisterClient(again) == 0);
}

TEST_CASE("audio_system: Unregister from the callback itself, no callback afterwards") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  g_unregister_self = 0x41550000;
  const uint32_t handle = Register(e, 5);
  REQUIRE(handle == 0x41550000);
  REQUIRE(WaitFor([&] { return g_unregister_result.load() != 0xFFFFFFFF; }, 1000ms));
  CHECK(g_unregister_result.load() == 0);
  const uint32_t after = g_calls;
  std::this_thread::sleep_for(100ms);
  CHECK(g_calls.load() == after);
  // the slot is free again
  const uint32_t again = Register(e, 6);
  CHECK(again == 0x41550000);
  CHECK(apu::UnregisterClient(again) == 0);
}

// Unregister from another thread while the callback is running: it returns only
// after the callback has exited.
TEST_CASE("audio_system: Unregister with the callback running waits for it to exit") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  g_sleep_ms = 5;
  int caught_inside = 0;
  for (int round = 0; round < 5; ++round) {
    const uint32_t handle = Register(e, 20 + round);
    REQUIRE(handle == 0x41550000);
    REQUIRE(WaitFor([&] { return g_inside.load(); }, 1000ms));
    const bool was_inside = g_inside.load();
    CHECK(apu::UnregisterClient(handle) == 0);
    const uint32_t entries = g_entries, exits = g_exits;
    const bool inside_after = g_inside.load();
    caught_inside += was_inside;
    INFO("round " << round << ": entries " << entries << ", exits " << exits);
    CHECK_FALSE(inside_after);
    CHECK(entries == exits);
    std::this_thread::sleep_for(20ms);
    CHECK(g_entries.load() == entries);  // no callback after the return
  }
  CHECK(caught_inside > 0);
  g_sleep_ms = 0;
}

// A BufferDone from the sink of a previous registration, called after the
// new registration in the same slot, gives no credits to the new one.
TEST_CASE("audio_system: late BufferDone from the previous registration is ignored") {
  Env& e = E();
  ResetCallback();
  g_saved_done.clear();
  REQUIRE(apu::Init({plat::AudioSinkKind::kNull, false, std::nullopt}, &SavingFactory));
  const uint64_t cb0 = Counter("audio_callbacks");
  const uint32_t first = Register(e, 30);
  REQUIRE(first == 0x41550000);
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb0 >= 16; }, 1000ms));
  CHECK(apu::UnregisterClient(first) == 0);
  REQUIRE(g_saved_done.size() == 1);

  const uint64_t cb1 = Counter("audio_callbacks");
  const uint32_t second = Register(e, 31);
  REQUIRE(second == 0x41550000);
  REQUIRE(g_saved_done.size() == 2);
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb1 >= 16; }, 1000ms));
  for (int i = 0; i < 10; ++i) g_saved_done[0]();  // old sink, already destroyed
  std::this_thread::sleep_for(100ms);
  CHECK(Counter("audio_callbacks") - cb1 == 16);
  // Counter-check: the current registration's BufferDone gives credits.
  for (int i = 0; i < 3; ++i) g_saved_done[1]();
  REQUIRE(WaitFor([&] { return Counter("audio_callbacks") - cb1 >= 19; }, 1000ms));
  std::this_thread::sleep_for(50ms);
  CHECK(Counter("audio_callbacks") - cb1 == 19);
  CHECK(apu::UnregisterClient(second) == 0);
  apu::Shutdown();
  g_saved_done.clear();
}

// A worker from a previous Init, left inside a callback past Shutdown,
// must not reset the new worker's state when it leaves: Unregister of the
// new client must still wait for its running callback.
TEST_CASE("audio_system: the worker of a previous Init does not touch the new worker's state") {
  Env& e = E();
  ResetCallback();
  g_hold = true;
  g_hold_entered = false;
  REQUIRE(apu::Init({plat::AudioSinkKind::kNull, false, std::nullopt}, &NullFactory));
  REQUIRE(Register(e, kHoldArg) == 0x41550000);
  REQUIRE(WaitFor([&] { return g_hold_entered.load(); }, 1000ms));
  apu::Shutdown();  // the old worker stays in the callback (1 s wait)

  ApuSession session;
  g_sleep_ms = 300;
  const uint32_t entries0 = g_entries;
  const uint32_t handle = Register(e, 40);
  REQUIRE(handle == 0x41550000);
  REQUIRE(WaitFor([&] { return g_entries.load() > entries0; }, 1000ms));
  g_hold = false;  // the old worker leaves the callback while the new one is inside
  std::this_thread::sleep_for(30ms);
  CHECK(apu::UnregisterClient(handle) == 0);
  // all started callbacks have finished (the old one and the new one)
  CHECK(g_exits.load() == g_entries.load());
  CHECK_FALSE(g_inside.load());
  g_sleep_ms = 0;
}

// Unregister waits without limit, but after ~2 s it writes it once to the log.
TEST_CASE("audio_system: Unregister blocked for over 2 s writes it to the log once") {
  Env& e = E();
  ResetCallback();
  const auto log_path = gow2_test::UniqueTempPath("gow2_audio_unreg_log");
  LogInit(log_path);
  {
    ApuSession session;
    g_hold = true;
    g_hold_entered = false;
    const uint32_t handle = Register(e, kHoldArg);
    REQUIRE(handle == 0x41550000);
    REQUIRE(WaitFor([&] { return g_hold_entered.load(); }, 1000ms));
    std::thread release([] {
      std::this_thread::sleep_for(2300ms);
      g_hold = false;
    });
    const auto t0 = Clock::now();
    CHECK(apu::UnregisterClient(handle) == 0);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    release.join();
    CHECK(ms >= 2200);
    CHECK(g_exits.load() == g_entries.load());
  }
  LogInit({});
  std::ifstream f(log_path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  f.close();
  const std::string log = ss.str();
  std::error_code ec;
  std::filesystem::remove(log_path, ec);
  const std::string line = "[audio] Unregister waiting for the callback (client 0)";
  const size_t first = log.find(line);
  CHECK(first != std::string::npos);
  CHECK(log.find(line, first + 1) == std::string::npos);
}

// Default factory with --audio xaudio2: XAudio2 if there is an endpoint,
// otherwise the fallback line and the null sink; the callbacks run either way.
TEST_CASE("audio_system: default factory with xaudio2, the callbacks run") {
  Env& e = E();
  ResetCallback();
  REQUIRE(apu::Init({plat::AudioSinkKind::kXAudio2, true, std::nullopt}));
  const uint64_t cb0 = Counter("audio_callbacks");
  g_samples = e.Alloc(plat::kAudioFrameFloats * 4);
  g_submit_handle = 0x41550000;
  g_submit = true;
  const uint32_t handle = Register(e, 50);
  REQUIRE(handle == 0x41550000);
  std::this_thread::sleep_for(1000ms);
  CHECK(apu::UnregisterClient(handle) == 0);
  g_submit = false;
  const uint64_t callbacks = Counter("audio_callbacks") - cb0;
  INFO("callbacks in 1 s: " << callbacks);
  CHECK(callbacks >= 16 + 150);
  CHECK(callbacks <= 16 + 230);
  apu::Shutdown();
}

TEST_CASE("audio_system: at most 8 clients, the ninth registration fails") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  std::vector<uint32_t> handles;
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t status = 0xFFFFFFFF;
    handles.push_back(Register(e, 10 + i, &status));
    CHECK(status == 0);
    CHECK(handles.back() == (0x41550000 | i));
  }
  uint32_t status = 0;
  CHECK(Register(e, 99, &status) == 0);
  CHECK(status == 0xC0000001);
  for (uint32_t h : handles) CHECK(apu::UnregisterClient(h) == 0);
}

TEST_CASE("audio_system: WAV capture of the submitted frames") {
  Env& e = E();
  ResetCallback();
  const auto path = gow2_test::UniqueTempPath("gow2_audio_capture");
  {
    apu::AudioOptions options;
    options.capture = path;
    options.mute = true;  // the capture does not change
    ApuSession session(options);
    const uint32_t handle = Register(e, 7);
    REQUIRE(handle != 0);
    const uint32_t samples = e.Alloc(plat::kAudioFrameFloats * 4);
    const uint64_t fr0 = Counter("audio_frames_submitted");
    for (int i = 0; i < 10; ++i) CHECK(apu::SubmitFrame(handle, samples) == 0);
    CHECK(Counter("audio_frames_submitted") - fr0 == 10);
    CHECK(apu::CaptureSampleFramesWritten() == 2560);
    CHECK(apu::UnregisterClient(handle) == 0);
  }
  std::error_code ec;
  CHECK(std::filesystem::file_size(path, ec) == 68 + 2560 * 24);
  std::filesystem::remove(path, ec);
}

TEST_CASE("audio_system: capture file cannot be opened, Init fails") {
  apu::AudioOptions options;
  options.capture = std::filesystem::path("Z:/this/folder/does/not/exist/x.wav");
  CHECK_FALSE(apu::Init(options, &NullFactory));
  apu::Shutdown();
}

TEST_CASE("audio_system: Shutdown with a callback running returns early") {
  Env& e = E();
  ResetCallback();
  REQUIRE(apu::Init({plat::AudioSinkKind::kNull, false, std::nullopt}, &NullFactory));
  g_sleep_ms = 50;
  const uint32_t start = g_calls;
  REQUIRE(Register(e, 8) != 0);
  REQUIRE(WaitFor([&] { return g_calls.load() > start; }, 1000ms));
  const auto t0 = Clock::now();
  apu::Shutdown();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  CHECK(ms < 200);
  const uint32_t after = g_calls;
  std::this_thread::sleep_for(100ms);
  CHECK(g_calls.load() == after);
  apu::Shutdown();  // idempotent
  g_sleep_ms = 0;
}

TEST_CASE("audio_system: XAudio* exports on the audio system") {
  Env& e = E();
  ResetCallback();
  ApuSession session;
  const uint32_t cb = e.Alloc(8), drv = e.Alloc(4), drv2 = e.Alloc(4);
  Put32(cb, kFnCallback);
  Put32(cb + 4, 0x1234);
  CHECK(Call(__imp__XAudioRegisterRenderDriverClient, {cb, drv}) == 0);
  CHECK(Call(__imp__XAudioRegisterRenderDriverClient, {cb, drv2}) == 0);
  CHECK((Get32(drv) & 0xFFFF0000) == 0x41550000);
  CHECK(Get32(drv) != Get32(drv2));
  const uint64_t fr0 = Counter("audio_frames_submitted");
  CHECK(Call(__imp__XAudioSubmitRenderDriverFrame, {Get32(drv), e.Alloc(plat::kAudioFrameFloats * 4)}) == 0);
  CHECK(Counter("audio_frames_submitted") - fr0 == 1);
  const uint32_t vol = e.Alloc(4), mask = e.Alloc(4);
  Put32(mask, 0xFFFFFFFF);
  CHECK(Call(__imp__XAudioGetVoiceCategoryVolume, {0, vol}) == 0);
  CHECK(Get32(vol) == 0x3F800000);  // 1.0f
  CHECK(Call(__imp__XAudioGetVoiceCategoryVolumeChangeMask, {Get32(drv), mask}) == 0);
  CHECK(Get32(mask) == 0);
  CHECK(Call(__imp__XAudioUnregisterRenderDriverClient, {Get32(drv)}) == 0);
  CHECK(Call(__imp__XAudioUnregisterRenderDriverClient, {Get32(drv2)}) == 0);
  // invalid handle: ignored
  CHECK(Call(__imp__XAudioUnregisterRenderDriverClient, {0x12345678}) == 0);
  CHECK(Call(__imp__XAudioSubmitRenderDriverFrame, {0x41550007, 0}) == 0);
}
