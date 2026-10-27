// 5.1 float WAV writer: format, values and header rewritten while writing.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "apu/wav_writer.h"
#include "temp_dir.h"

namespace {
std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
uint32_t U32(const std::vector<uint8_t>& b, size_t o) {
  uint32_t v;
  std::memcpy(&v, &b[o], 4);
  return v;
}
uint16_t U16(const std::vector<uint8_t>& b, size_t o) {
  uint16_t v;
  std::memcpy(&v, &b[o], 2);
  return v;
}
}  // namespace

TEST_CASE("wav_writer: extensible format with 6 float channels and sample values") {
  const auto path = gow2_test::UniqueTempPath("wav_writer_fmt").replace_extension(".wav");
  {
    auto w = apu::WavWriter::Create(path);
    REQUIRE(w);
    std::vector<float> frame(256 * 6);
    for (int i = 0; i < 256; ++i)
      for (int c = 0; c < 6; ++c) frame[i * 6 + c] = float(c) + float(i) / 1000.0f;
    w->Append(frame);
    w->Append(frame);
    CHECK(w->SampleFramesWritten() == 512);
    w->Close();
    w->Close();  // idempotent
  }
  const auto b = ReadAll(path);
  REQUIRE(b.size() == 68 + 2 * 256 * 24);
  CHECK(std::memcmp(&b[0], "RIFF", 4) == 0);
  CHECK(U32(b, 4) == b.size() - 8);
  CHECK(std::memcmp(&b[8], "WAVEfmt ", 8) == 0);
  CHECK(U32(b, 16) == 40);
  CHECK(U16(b, 20) == 0xFFFE);
  CHECK(U16(b, 22) == 6);
  CHECK(U32(b, 24) == 48000);
  CHECK(U32(b, 28) == 1152000);
  CHECK(U16(b, 32) == 24);
  CHECK(U16(b, 34) == 32);
  CHECK(U16(b, 36) == 22);
  CHECK(U16(b, 38) == 32);
  CHECK(U32(b, 40) == 0x3F);
  const uint8_t guid[16] = {0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
  CHECK(std::memcmp(&b[44], guid, 16) == 0);
  CHECK(std::memcmp(&b[60], "data", 4) == 0);
  CHECK(U32(b, 64) == 12288);
  float v;
  std::memcpy(&v, &b[68 + (3 * 6 + 4) * 4], 4);
  CHECK(v == 4.003f);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST_CASE("wav_writer: Create in a nonexistent folder returns nullptr") {
  const auto dir = gow2_test::UniqueTempPath("wav_writer_nodir");
  CHECK(apu::WavWriter::Create(dir / "x.wav") == nullptr);
}

TEST_CASE("wav_writer: the on-disk header is valid up to the last second without Close") {
  const auto path = gow2_test::UniqueTempPath("wav_writer_hdr").replace_extension(".wav");
  auto w = apu::WavWriter::Create(path);
  REQUIRE(w);
  const std::vector<float> frame(256 * 6, 0.5f);
  uint64_t total = 0;
  while (total < 50000) {
    w->Append(frame);
    total += 256;
  }
  const auto b = ReadAll(path);  // the writer is still open
  REQUIRE(b.size() >= 68);
  CHECK(U32(b, 64) == 48000u * 24u);
  CHECK(U32(b, 4) == 48000u * 24u + 60u);
  w->Close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
