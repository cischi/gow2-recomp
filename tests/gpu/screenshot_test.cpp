// Screenshots without a GPU: --screenshot S:file argument (gpu/screenshot.h) and
// writing of the 24-bit BMP (gpu/bmp.h).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gpu/bmp.h"
#include "gpu/screenshot.h"
#include "temp_dir.h"

namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

uint32_t Le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

}  // namespace

TEST_CASE("screenshot: ParseScreenshotArg splits seconds and file at the first ':'", "[screenshot]") {
  const auto a = gpu::ParseScreenshotArg(L"45:shot.bmp");
  REQUIRE(a);
  CHECK(a->seconds == 45);
  CHECK(a->file == std::filesystem::path(L"shot.bmp"));

  const auto b = gpu::ParseScreenshotArg(L"0:C:\\x\\s.bmp");
  REQUIRE(b);
  CHECK(b->seconds == 0);
  CHECK(b->file == std::filesystem::path(L"C:\\x\\s.bmp"));

  const auto max = gpu::ParseScreenshotArg(L"86400:a");
  REQUIRE(max);
  CHECK(max->seconds == 86400);
}

TEST_CASE("screenshot: ParseScreenshotArg rejects malformed arguments", "[screenshot]") {
  for (const wchar_t* bad : {L"45", L":a.bmp", L"45:", L"-1:a", L"86401:a", L"", L"4x:a", L"+5:a", L" 5:a",
                             L"99999999999999999999:a"}) {
    INFO("argument: " << std::filesystem::path(bad).string());
    CHECK_FALSE(gpu::ParseScreenshotArg(bad));
  }
}

TEST_CASE("screenshot: WriteBmp24 3x2 bottom-up, BGR, rows aligned to 4 bytes", "[screenshot]") {
  gow2_test::TempDir dir("gow2_bmp");
  const auto file = dir.path / "s.bmp";
  // row 0 (top): red, green, blue; row 1: white, black, (0x12,0x34,0x56)
  const std::array<uint32_t, 6> px = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF, 0x00000000, 0xFF123456};
  REQUIRE(gpu::WriteBmp24(file, 3, 2, px));
  const auto b = ReadAll(file);
  REQUIRE(b.size() == 78);  // 54 + 2 rows * 12 bytes (9 + 3 padding)
  CHECK(b[0] == 'B');
  CHECK(b[1] == 'M');
  CHECK(Le32(b, 2) == 78);   // bfSize
  CHECK(Le32(b, 10) == 54);  // bfOffBits
  CHECK(Le32(b, 14) == 40);  // biSize
  CHECK(Le32(b, 18) == 3);   // biWidth
  CHECK(Le32(b, 22) == 2);   // biHeight positive: bottom-up
  CHECK((b[26] | b[27] << 8) == 1);   // biPlanes
  CHECK((b[28] | b[29] << 8) == 24);  // biBitCount
  CHECK(Le32(b, 30) == 0);   // BI_RGB
  CHECK(Le32(b, 34) == 24);  // biSizeImage
  // first row of the file = last of the input, in BGR
  const uint8_t row0[12] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x56, 0x34, 0x12, 0, 0, 0};
  const uint8_t row1[12] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0, 0, 0};
  CHECK(std::memcmp(b.data() + 54, row0, 12) == 0);
  CHECK(std::memcmp(b.data() + 66, row1, 12) == 0);
}

TEST_CASE("screenshot: WriteBmp24 rejects inconsistent dimensions and nonexistent folders", "[screenshot]") {
  gow2_test::TempDir dir("gow2_bmp");
  const std::array<uint32_t, 4> px{};
  CHECK_FALSE(gpu::WriteBmp24(dir.path / "a.bmp", 3, 2, px));  // 6 pixels needed
  CHECK_FALSE(gpu::WriteBmp24(dir.path / "b.bmp", 0, 0, {}));
  CHECK_FALSE(gpu::WriteBmp24(dir.path / "does not exist" / "c.bmp", 2, 2, px));
}

TEST_CASE("screenshot: WritePpm10 2x1, header and 16-bit big-endian channels", "[screenshot]") {
  gow2_test::TempDir dir("gow2_ppm");
  const auto file = dir.path / "s.ppm";
  // pixel 0: r=0x3FF g=0x200 b=0x001 (alpha 0); pixel 1: black
  const std::array<uint32_t, 2> px = {0x3FFu | (0x200u << 10) | (0x001u << 20), 0u};
  REQUIRE(gpu::WritePpm10(file, 2, 1, px));
  const auto b = ReadAll(file);
  const std::string head = "P6\n2 1\n1023\n";
  const uint8_t body[12] = {0x03, 0xFF, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  REQUIRE(b.size() == head.size() + sizeof(body));
  CHECK(std::memcmp(b.data(), head.data(), head.size()) == 0);
  CHECK(std::memcmp(b.data() + head.size(), body, sizeof(body)) == 0);
}

TEST_CASE("screenshot: WritePpm10 ignores alpha", "[screenshot]") {
  gow2_test::TempDir dir("gow2_ppm");
  const uint32_t rgb = 0x3FFu | (0x200u << 10) | (0x001u << 20);
  const std::array<uint32_t, 1> a = {rgb}, b = {rgb | (3u << 30)};
  REQUIRE(gpu::WritePpm10(dir.path / "a.ppm", 1, 1, a));
  REQUIRE(gpu::WritePpm10(dir.path / "b.ppm", 1, 1, b));
  CHECK(ReadAll(dir.path / "a.ppm") == ReadAll(dir.path / "b.ppm"));
}

TEST_CASE("screenshot: WritePpm10 rejects inconsistent dimensions and nonexistent folders", "[screenshot]") {
  gow2_test::TempDir dir("gow2_ppm");
  const std::array<uint32_t, 4> px{};
  CHECK_FALSE(gpu::WritePpm10(dir.path / "a.ppm", 3, 2, px));  // 6 pixels needed
  CHECK_FALSE(gpu::WritePpm10(dir.path / "b.ppm", 0, 0, {}));
  CHECK_FALSE(gpu::WritePpm10(dir.path / "does not exist" / "c.ppm", 2, 2, px));
}
