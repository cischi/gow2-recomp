// game_data: SHA-256, default.xex check, extraction marker, data folder resolution.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "game_data.h"
#include "mini_xex.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

void Write(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << bytes;
}

// Sets GOW2_SKIP_XEX_HASH for one scope, then restores the previous value.
struct SkipEnv {
  std::string old;
  bool had = false;
  explicit SkipEnv(const char* value) {
    if (const char* v = std::getenv("GOW2_SKIP_XEX_HASH")) {
      had = true;
      old = v;
    }
    _putenv_s("GOW2_SKIP_XEX_HASH", value);
  }
  ~SkipEnv() { _putenv_s("GOW2_SKIP_XEX_HASH", had ? old.c_str() : ""); }
};

}  // namespace

TEST_CASE("sha256_bytes_and_file", "[game_data]") {
  gow2_test::TempDir tmp("gow2_gd_sha");
  const std::byte abc[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  CHECK(game_data::Sha256Bytes(abc) == kAbc);
  Write(tmp.path / "abc", "abc");
  CHECK(game_data::Sha256File(tmp.path / "abc") == kAbc);
  CHECK_FALSE(game_data::Sha256File(tmp.path / "missing").has_value());
}

TEST_CASE("check_xex", "[game_data]") {
  SkipEnv env("");
  gow2_test::TempDir tmp("gow2_gd_xex");
  const std::string xex = gow2_test::MiniXex("game");
  const auto image = game_data::XexImageSha256(std::as_bytes(std::span(xex.data(), xex.size())));
  REQUIRE(image.has_value());
  CHECK(game_data::CheckXex(tmp.path, *image) == game_data::XexCheck::kMissing);
  Write(tmp.path / "default.xex", gow2_test::MiniXex("game", 'k'));  // same image, other container
  CHECK(game_data::CheckXex(tmp.path, *image) == game_data::XexCheck::kOk);
  CHECK(game_data::CheckXex(tmp.path, "00") == game_data::XexCheck::kMismatch);
  Write(tmp.path / "default.xex", "abc");  // not an XEX
  CHECK(game_data::CheckXex(tmp.path, *image) == game_data::XexCheck::kMismatch);
  _putenv_s("GOW2_SKIP_XEX_HASH", "1");
  CHECK(game_data::CheckXex(tmp.path, "00") == game_data::XexCheck::kOk);
  fs::remove(tmp.path / "default.xex");
  CHECK(game_data::CheckXex(tmp.path, "00") == game_data::XexCheck::kMissing);
}

TEST_CASE("check_xex_other_game_hash", "[game_data]") {
  SkipEnv env("");
  gow2_test::TempDir tmp("gow2_gd_xex_other");
  Write(tmp.path / "default.xex", "any xex");
  CHECK(game_data::CheckXex(tmp.path, std::string(64, '0')) == game_data::XexCheck::kMismatch);
}

TEST_CASE("extraction_complete", "[game_data]") {
  gow2_test::TempDir tmp("gow2_gd_ext");
  CHECK_FALSE(game_data::ExtractionComplete(tmp.path));
  Write(tmp.path / "default.xex", "x");
  CHECK_FALSE(game_data::ExtractionComplete(tmp.path));
  Write(tmp.path / ".extract_ok", "1\n");
  CHECK(game_data::ExtractionComplete(tmp.path));
}

TEST_CASE("resolve_data_dir", "[game_data]") {
  gow2_test::TempDir tmp("gow2_gd_dir");
  const fs::path exe_dir = tmp.path / "exe";
  fs::create_directories(exe_dir);
  const fs::path old_cwd = fs::current_path();
  const fs::path cwd = tmp.path / "cwd";
  fs::create_directories(cwd);
  fs::current_path(cwd);
  for (const char* name : {"game_data", "game_data_gow3"}) {
    CHECK(game_data::ResolveDataDir(exe_dir, name) == exe_dir / name);
    fs::create_directories(cwd / name);
    CHECK(game_data::ResolveDataDir(exe_dir, name) == fs::path(name));
  }
  fs::current_path(old_cwd);
}
