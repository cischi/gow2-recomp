// Null audio sink: 187.5 buffers/s pacing, 64-entry queue, clean shutdown.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "platform/platform.h"

namespace {
const std::array<float, plat::kAudioFrameFloats> kFrame{};
}

TEST_CASE("null_audio_out: 187.5 buffers/s pacing with immediate resubmission") {
  std::atomic<int> calls{0};
  std::atomic<plat::AudioOut*> sink_ptr{nullptr};
  auto out = plat::CreateNullAudioOut([&] {
    ++calls;
    if (auto* s = sink_ptr.load()) s->Submit(kFrame);
  });
  REQUIRE(out->Kind() == plat::AudioSinkKind::kNull);
  sink_ptr = out.get();
  for (uint32_t i = 0; i < plat::kAudioMaxQueued; ++i) REQUIRE(out->Submit(kFrame));
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  sink_ptr = nullptr;
  const int n = calls.load();
  INFO("callbacks in 2.0 s: " << n);
  CHECK(n >= 338);
  CHECK(n <= 412);
}

TEST_CASE("null_audio_out: the 65th Submit without consumption fails") {
  auto out = plat::CreateNullAudioOut([] {});
  int accepted = 0;
  for (uint32_t i = 0; i < plat::kAudioMaxQueued; ++i) accepted += out->Submit(kFrame);
  CHECK(accepted == 64);
  // The sink thread has not consumed anything yet (first buffer after 5.3 ms).
  CHECK_FALSE(out->Submit(kFrame));
}

TEST_CASE("null_audio_out: empty queue, no callback") {
  std::atomic<int> calls{0};
  auto out = plat::CreateNullAudioOut([&] { ++calls; });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(calls.load() == 0);
}

TEST_CASE("null_audio_out: no callback after destruction") {
  std::atomic<int> calls{0};
  {
    auto out = plat::CreateNullAudioOut([&] { ++calls; });
    for (uint32_t i = 0; i < plat::kAudioMaxQueued; ++i) out->Submit(kFrame);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const int after = calls.load();
  CHECK(after > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(calls.load() == after);
}
