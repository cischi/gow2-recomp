// XAudio2 2.9 output (plat::CreateXAudio2AudioOut): real engine pacing,
// fallback to null pacing after a critical error (Review Focus 5), clean
// shutdown. Cases that need an audio endpoint SKIP if the engine cannot be
// created; the creation itself is always tested. Silence buffers only.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "log.h"
#include "platform/platform.h"
#include "temp_dir.h"

namespace {

using namespace std::chrono_literals;

const std::array<float, plat::kAudioFrameFloats> kSilence{};

// XAudio2 sink with immediate resubmission in on_done; nullptr if there is no
// endpoint (or DLL), with hr in the SKIP message.
struct Resubmitting {
  std::atomic<int> calls{0};
  std::atomic<plat::AudioOut*> sink{nullptr};
  std::unique_ptr<plat::AudioOut> out;
  uint32_t hr = 0;

  bool Create() {
    out = plat::CreateXAudio2AudioOut(
        [this] {
          ++calls;
          if (auto* s = sink.load()) s->Submit(kSilence);
        },
        &hr);
    return out != nullptr;
  }
  void Fill() {
    sink = out.get();
    for (uint32_t i = 0; i < plat::kAudioMaxQueued; ++i) REQUIRE(out->Submit(kSilence));
  }
};

std::string ReadFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++n;
  return n;
}

}  // namespace

TEST_CASE("xaudio2_out: creation does not throw, nullptr only with a non-zero hr") {
  uint32_t hr = 0;
  std::unique_ptr<plat::AudioOut> out;
  REQUIRE_NOTHROW(out = plat::CreateXAudio2AudioOut([] {}, &hr));
  if (!out) {
    INFO("hr = " << std::hex << hr);
    CHECK(hr != 0);
  } else {
    CHECK(out->Kind() == plat::AudioSinkKind::kXAudio2);
    out->SetMuted(true);
    out->SetMuted(false);
  }
  // null hr_out allowed.
  REQUIRE_NOTHROW(out = plat::CreateXAudio2AudioOut([] {}, nullptr));
  out.reset();
}

TEST_CASE("xaudio2_out: 64 resubmitted buffers, real pacing of 187.5/s") {
  Resubmitting r;
  if (!r.Create()) SKIP("xaudio2 not available, hr=0x" << std::hex << r.hr);
  r.out->SetMuted(true);
  r.Fill();
  std::this_thread::sleep_for(2000ms);
  r.sink = nullptr;
  const int n = r.calls.load();
  INFO("on_done in 2.0 s: " << n);
  CHECK(n >= 338);
  CHECK(n <= 412);
}

TEST_CASE("xaudio2_out: critical error midway, the callbacks continue at null pacing") {
  const auto log_path = gow2_test::UniqueTempPath("gow2_xaudio2_log");
  LogInit(log_path);
  int after = 0;
  {
    Resubmitting r;
    if (!r.Create()) {
      LogInit({});
      std::error_code ec;
      std::filesystem::remove(log_path, ec);
      SKIP("xaudio2 not available, hr=0x" << std::hex << r.hr);
    }
    r.out->SetMuted(true);
    r.Fill();
    std::this_thread::sleep_for(500ms);
    plat::SimulateAudioCriticalError(*r.out);
    plat::SimulateAudioCriticalError(*r.out);  // a second error changes nothing
    const int at_error = r.calls.load();
    std::this_thread::sleep_for(1500ms);
    after = r.calls.load() - at_error;
    r.sink = nullptr;
    CHECK(r.out->Kind() == plat::AudioSinkKind::kXAudio2);
  }
  LogInit({});
  const std::string log = ReadFile(log_path);
  std::error_code ec;
  std::filesystem::remove(log_path, ec);
  INFO("on_done in the 1.5 s after the error: " << after);
  CHECK(after >= 253);
  CHECK(after <= 310);
  CHECK(CountOccurrences(log, "[audio] xaudio2 critical error (hr=0x") == 1);
  CHECK(CountOccurrences(log, "), using the null sink") == 1);
}

TEST_CASE("xaudio2_out: no on_done after destruction") {
  for (bool critical : {false, true}) {
    Resubmitting r;
    if (!r.Create()) SKIP("xaudio2 not available, hr=0x" << std::hex << r.hr);
    r.out->SetMuted(true);
    r.Fill();
    std::this_thread::sleep_for(50ms);
    if (critical) plat::SimulateAudioCriticalError(*r.out);
    std::this_thread::sleep_for(50ms);
    r.out.reset();  // on_done resubmits until the end: sink stays set
    const int at_return = r.calls.load();
    INFO("critical " << critical << ", on_done " << at_return);
    CHECK(at_return > 0);
    std::this_thread::sleep_for(100ms);
    CHECK(r.calls.load() == at_return);
  }
}

TEST_CASE("xaudio2_out: failed SubmitSourceBuffer, the buffer stays accepted and switches to null pacing") {
  const auto log_path = gow2_test::UniqueTempPath("gow2_xaudio2_submit_log");
  LogInit(log_path);
  std::atomic<int> calls{0};
  uint32_t hr = 0;
  std::unique_ptr<plat::AudioOut> out = plat::CreateXAudio2AudioOut([&calls] { ++calls; }, &hr);
  if (!out) {
    LogInit({});
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
    SKIP("xaudio2 not available, hr=0x" << std::hex << hr);
  }
  out->SetMuted(true);
  constexpr int kBuffers = 10;
  for (int i = 0; i < kBuffers; ++i) {
    if (i == 4) plat::SimulateAudioSubmitFailure(*out);
    CHECK(out->Submit(kSilence));
  }
  // Every accepted buffer gives exactly one on_done (no credit lost):
  // 10 buffers at null pacing take ~53 ms.
  std::this_thread::sleep_for(400ms);
  CHECK(calls.load() == kBuffers);
  out.reset();
  CHECK(calls.load() == kBuffers);
  LogInit({});
  const std::string log = ReadFile(log_path);
  std::error_code ec;
  std::filesystem::remove(log_path, ec);
  CHECK(CountOccurrences(log, "[audio] xaudio2 critical error (hr=0x88960004), using the null sink") == 1);
}
