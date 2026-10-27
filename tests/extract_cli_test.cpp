#define _CRT_SECURE_NO_WARNINGS
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "run_extract.h"
#include "xdvdfs_builder.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

struct TempDir : gow2_test::TempDir {
  explicit TempDir(const char* tag) : gow2_test::TempDir(std::string("gow2_extract_") + tag) {}
};

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& d) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}
void WriteText(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}
std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

TEST_CASE("extracts into a folder with spaces and brackets") {
  TempDir t("spaces");
  auto iso = t.path / "Gears of War 2 [360]" / "DVD.iso";
  WriteBytes(iso, BuildImage({{"default.xex", "XEX2"}, {"CookedXenon/a.xxx", "abc"}}, 0xFD90000));
  auto out = t.path / "out [x]";
  std::ostringstream log;
  REQUIRE(RunExtract(iso, out, log) == 0);
  CHECK(ReadAll(out / "CookedXenon" / "a.xxx") == "abc");
  CHECK(ReadAll(out / "default.xex") == "XEX2");
  CHECK(log.str().find("2 files extracted") != std::string::npos);
  CHECK(ReadAll(out / ".extract_ok") == "2\n");
}

TEST_CASE("marker absent on error and partial file removed") {
  TempDir t("partial");
  auto iso = t.path / "DVD.iso";
  auto img = BuildImage({{"default.xex", "XEX2"}, {"big.bin", std::string(5000, 'z')}}, 0xFD90000);
  // big.bin's size in the directory exceeds the end of the image: the read fails
  const std::string name = "big.bin";
  bool patched = false;
  for (size_t i = 14; i + name.size() <= img.size(); ++i) {
    if (img[i - 1] == name.size() && std::equal(name.begin(), name.end(), img.begin() + i)) {
      const uint32_t huge = 0x20000000;  // 512 MiB
      for (int k = 0; k < 4; ++k) img[i - 6 + k] = uint8_t(huge >> (8 * k));
      patched = true;
      break;
    }
  }
  REQUIRE(patched);
  WriteBytes(iso, img);
  auto out = t.path / "out";
  WriteText(out / ".extract_ok", "old\n");  // from a previous extraction
  std::ostringstream log;
  CHECK(RunExtract(iso, out, log) == 1);
  CHECK(log.str().find("read failed") != std::string::npos);
  CHECK_FALSE(fs::exists(out / ".extract_ok"));
  CHECK_FALSE(fs::exists(out / "big.bin"));
}

TEST_CASE("overwrites but does not delete") {
  TempDir t("overwrite");
  auto iso = t.path / "DVD.iso";
  WriteBytes(iso, BuildImage({{"default.xex", "XEX2"}}, 0xFD90000));
  auto out = t.path / "out";
  WriteText(out / "default.xex", "OLD");
  WriteText(out / "extra.txt", "keep");
  std::ostringstream log;
  REQUIRE(RunExtract(iso, out, log) == 0);
  CHECK(ReadAll(out / "default.xex") == "XEX2");
  CHECK(fs::exists(out / "extra.txt"));
}

TEST_CASE("nonexistent ISO") {
  TempDir t("missing");
  std::ostringstream log;
  CHECK(RunExtract("nope.iso", t.path / "out", log) == 1);
  CHECK(log.str().find("nope.iso") != std::string::npos);
}

TEST_CASE("rejects names that escape out_dir") {
  for (const std::string evil : {"..", "a\\", "a:", "/x", "a\\..\\..\\x"}) {
    DYNAMIC_SECTION("name " << evil) {
      TempDir t("evil");
      // placeholder of the same length: the image layout does not change
      const std::string placeholder(evil.size(), 'y');
      auto img = BuildImage({{placeholder, "abc"}}, 0xFD90000);
      bool patched = false;
      for (size_t i = 1; i + placeholder.size() <= img.size(); ++i) {
        if (img[i - 1] == placeholder.size() &&
            std::equal(placeholder.begin(), placeholder.end(), img.begin() + i)) {
          std::copy(evil.begin(), evil.end(), img.begin() + i);
          patched = true;
          break;
        }
      }
      REQUIRE(patched);
      auto iso = t.path / "DVD.iso";
      WriteBytes(iso, img);
      std::ostringstream log;
      CHECK(RunExtract(iso, t.path / "out", log) == 1);
      CHECK(log.str().find(evil) != std::string::npos);
      CHECK_FALSE(fs::exists(t.path / "out"));  // nothing written: validation before creating anything
    }
  }
}

TEST_CASE("real ISO", "[real]") {
  const char* iso = std::getenv("GOW2_ISO");
  if (!iso) SKIP("GOW2_ISO not set");
  TempDir t("real");
  std::ostringstream log;
  REQUIRE(RunExtract(fs::path(iso), t.path / "out", log) == 0);
  CHECK(ReadAll(t.path / "out" / "default.xex").substr(0, 4) == "XEX2");
}

TEST_CASE("ReadIsoFile, IsoPayloadBytes and progress") {
  TempDir t("hooks");
  auto iso = t.path / "DVD.iso";
  const std::string big(5u << 20, 'q');  // two 4 MiB chunks
  WriteBytes(iso, BuildImage({{"default.xex", "XEX2"}, {"sub/big.bin", big}}, 0xFD90000));

  auto xex = ReadIsoFile(iso, "default.xex");
  REQUIRE(xex);
  CHECK(std::string(reinterpret_cast<const char*>(xex->data()), xex->size()) == "XEX2");
  CHECK_FALSE(ReadIsoFile(iso, "missing.xex"));
  CHECK_FALSE(ReadIsoFile(iso, "big.bin"));  // root only
  auto junk = t.path / "junk.iso";
  WriteText(junk, std::string(4096, 'x'));
  CHECK_FALSE(ReadIsoFile(junk, "default.xex"));
  CHECK_FALSE(IsoPayloadBytes(junk));
  CHECK(IsoPayloadBytes(iso) == 4 + big.size());

  std::vector<std::pair<uint64_t, uint64_t>> calls;
  ExtractHooks h;
  h.progress = [&](uint64_t d, uint64_t tot) { calls.emplace_back(d, tot); };
  std::ostringstream log;
  REQUIRE(RunExtract(iso, t.path / "out", log, h) == 0);
  REQUIRE_FALSE(calls.empty());
  CHECK(calls.back().first == calls.back().second);
  CHECK(calls.back().second == 4 + big.size());

  // Cancel once 4 MiB are done: whatever the listing order, big.bin (two chunks) is
  // cancelled after being opened and its first chunk written, so it must be removed.
  uint64_t seen = 0;
  ExtractHooks c;
  c.progress = [&](uint64_t d, uint64_t) { seen = d; };
  c.cancelled = [&] { return seen >= (4u << 20); };
  auto out2 = t.path / "out2";
  CHECK(RunExtract(iso, out2, log, c) == 3);
  CHECK_FALSE(fs::exists(out2 / ".extract_ok"));
  CHECK_FALSE(fs::exists(out2 / "sub" / "big.bin"));
}

TEST_CASE("final progress call even with only empty files") {
  TempDir t("empty");
  auto iso = t.path / "DVD.iso";
  WriteBytes(iso, BuildImage({{"default.xex", ""}}, 0xFD90000));
  bool last = false;
  ExtractHooks h;
  h.progress = [&](uint64_t d, uint64_t tot) { last = (d == 0 && tot == 0); };
  std::ostringstream log;
  REQUIRE(RunExtract(iso, t.path / "out", log, h) == 0);
  CHECK(last);
}
