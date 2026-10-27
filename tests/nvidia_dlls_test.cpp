// nvidia_dlls::Install with a fake Fetch (no network).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <array>
#include <fstream>
#include <set>
#include <string>

#include "game_data.h"
#include "launcher/nvidia_dlls.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

void WriteFile(const fs::path& p, const std::string& bytes) { std::ofstream(p, std::ios::binary) << bytes; }

std::string HashOf(const std::string& bytes) {
  gow2_test::TempDir tmp("nvidia_hash");
  WriteFile(tmp.path / "x", bytes);
  return *game_data::Sha256File(tmp.path / "x");
}

struct Fixture {
  std::string hash_a = HashOf("aaaa"), hash_b = HashOf("bbbb");
  std::array<nvidia_dlls::Dll, 2> dlls{{{L"a", "a.dll", hash_a}, {L"b", "b.dll", hash_b}}};
  int fetches = 0;
  optiscaler::Fetch fetch_for(std::string a, std::string b, int fail_at = -1) {
    return [=, this](std::wstring_view url, const fs::path& dest) -> std::optional<std::string> {
      if (fetches++ == fail_at) return "network down";
      WriteFile(dest, url == L"a" ? a : b);
      return std::nullopt;
    };
  }
  static std::set<std::string> Names(const fs::path& d) {
    std::set<std::string> n;
    for (const auto& e : fs::directory_iterator(d)) n.insert(e.path().filename().string());
    return n;
  }
};

}  // namespace

TEST_CASE("nvidia_dlls: a cancel from an earlier install does not abort a later download", "[nvidia_dlls]") {
  Fixture f;
  gow2_test::TempDir dir("nvidia_cancel");
  optiscaler::CancelInstall();
  bool cancelled_during_fetch = true;
  const optiscaler::Fetch fetch = [&](std::wstring_view url, const fs::path& dest) -> std::optional<std::string> {
    cancelled_during_fetch = optiscaler::CancelRequested();
    return f.fetch_for("aaaa", "bbbb")(url, dest);
  };
  CHECK_FALSE(nvidia_dlls::Install(dir.path, fetch, f.dlls));
  CHECK_FALSE(cancelled_during_fetch);
}

TEST_CASE("nvidia_dlls: right hashes install both files and leave no .download", "[nvidia_dlls]") {
  Fixture f;
  gow2_test::TempDir dir("nvidia_ok");
  CHECK_FALSE(nvidia_dlls::Install(dir.path, f.fetch_for("aaaa", "bbbb"), f.dlls));
  CHECK(Fixture::Names(dir.path) == std::set<std::string>{"a.dll", "b.dll"});
}

TEST_CASE("nvidia_dlls: wrong hash is an error and installs nothing", "[nvidia_dlls]") {
  Fixture f;
  gow2_test::TempDir dir("nvidia_bad");
  const auto err = nvidia_dlls::Install(dir.path, f.fetch_for("aaaa", "tampered"), f.dlls);
  REQUIRE(err);
  CHECK(Fixture::Names(dir.path).empty());
}

TEST_CASE("nvidia_dlls: fetch failing on the second DLL leaves the folder as before", "[nvidia_dlls]") {
  Fixture f;
  gow2_test::TempDir dir("nvidia_fail2");
  WriteFile(dir.path / "keep.txt", "k");
  const auto err = nvidia_dlls::Install(dir.path, f.fetch_for("aaaa", "bbbb", 1), f.dlls);
  REQUIRE(err);
  CHECK(Fixture::Names(dir.path) == std::set<std::string>{"keep.txt"});
}

TEST_CASE("nvidia_dlls: present DLL with the right hash is skipped, wrong one replaced", "[nvidia_dlls]") {
  Fixture f;
  gow2_test::TempDir dir("nvidia_skip");
  WriteFile(dir.path / "a.dll", "aaaa");
  WriteFile(dir.path / "b.dll", "old");
  CHECK_FALSE(nvidia_dlls::Install(dir.path, f.fetch_for("aaaa", "bbbb"), f.dlls));
  CHECK(f.fetches == 1);
  CHECK(HashOf("bbbb") == *game_data::Sha256File(dir.path / "b.dll"));
  CHECK(Fixture::Names(dir.path) == std::set<std::string>{"a.dll", "b.dll"});
}
