// gow2::hooks glue of Xenia's APU (APU patches 0007/0008, runtime/apu/xma_hooks.cpp):
// summary counters and one log line per failing context.
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "apu/shims/gow2_apu_hooks.h"
#include "kernel/stats.h"
#include "log.h"
#include "temp_dir.h"

namespace {

uint64_t SummaryValue(const std::string& key) {
  const std::string summary = kernel::stats::Summary();
  const std::string prefix = "[summary] " + key + "=";
  const size_t at = summary.find(prefix);
  REQUIRE(at != std::string::npos);
  return std::stoull(summary.substr(at + prefix.size()));
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("xenia_apu: OnXmaFrameDecoded counts xma_frames_decoded") {
  const uint64_t before = SummaryValue("xma_frames_decoded");
  gow2::hooks::OnXmaFrameDecoded();
  gow2::hooks::OnXmaFrameDecoded();
  CHECK(SummaryValue("xma_frames_decoded") == before + 2);
}

TEST_CASE("xenia_apu: OnXmaDecodeError counts every error and logs one line per context") {
  gow2_test::TempDir dir("xma_hooks");
  const auto log_path = dir.path / "log.txt";
  LogInit(log_path);
  const uint64_t before = SummaryValue("xma_decode_errors");
  gow2::hooks::OnXmaDecodeError(5);
  gow2::hooks::OnXmaDecodeError(5);
  gow2::hooks::OnXmaDecodeError(319);
  gow2::hooks::OnXmaDecodeError(4000);  // outside the 320 contexts: counted, no crash
  gow2::hooks::OnXmaDecodeError(4000);
  LogInit({});
  CHECK(SummaryValue("xma_decode_errors") == before + 5);

  std::ifstream in(log_path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string log = ss.str();
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 5\n") == 1);
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 319\n") == 1);
  CHECK(CountOccurrences(log, "[apu] XMA decode error context 4000\n") == 1);
}
