// launcher::CheckIso: ISO validation before extraction.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <thread>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "game_data.h"
#include "launcher/game_setup.h"
#include "mini_xex.h"
#include "temp_dir.h"
#include "xdvdfs_builder.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kPlenty = 1ull << 40;

fs::path WriteIso(const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& files) {
  fs::create_directories(dir);
  const auto img = BuildImage(files, 0xFD90000);
  const auto p = dir / "game.iso";
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size()));
  return p;
}

const std::string kXex = gow2_test::MiniXex("game");

std::string ImageHashOf(const std::string& xex) {
  return game_data::XexImageSha256(std::as_bytes(std::span(xex.data(), xex.size()))).value();
}

}  // namespace

TEST_CASE("CheckIso: matching default.xex is accepted", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_ok");
  const auto iso = WriteIso(t.path, {{"default.xex", kXex}, {"a.bin", "abc"}});
  CHECK(launcher::CheckIso(iso, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kOk);
}

TEST_CASE("CheckIso: another container with the same image is accepted", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_region");
  const std::string other_region = gow2_test::MiniXex("game", 'k');
  REQUIRE(other_region != kXex);
  const auto iso = WriteIso(t.path, {{"default.xex", other_region}});
  CHECK(launcher::CheckIso(iso, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kOk);
}

TEST_CASE("CheckIso: other edition", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_edition");
  const auto iso = WriteIso(t.path, {{"default.xex", gow2_test::MiniXex("other game")}});
  CHECK(launcher::CheckIso(iso, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kWrongEdition);
  // Not an XEX at all: also another edition, never a crash.
  const auto junk = WriteIso(t.path / "junk", {{"default.xex", "XEX2"}});
  CHECK(launcher::CheckIso(junk, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kWrongEdition);
}

TEST_CASE("CheckIso: no default.xex", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_noxex");
  const auto iso = WriteIso(t.path, {{"a.bin", "abc"}});
  CHECK(launcher::CheckIso(iso, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kNoXex);
}

TEST_CASE("CheckIso: not an ISO", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_text");
  fs::create_directories(t.path);
  const auto p = t.path / "notes.iso";
  std::ofstream(p) << "hello";
  CHECK(launcher::CheckIso(p, kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kNotAnIso);
  CHECK(launcher::CheckIso(t.path / "missing.iso", kPlenty, ImageHashOf(kXex)) == launcher::IsoCheck::kNotAnIso);
}

TEST_CASE("CheckIso: not enough free space", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_space");
  const auto iso = WriteIso(t.path, {{"default.xex", kXex}});
  const std::string h = ImageHashOf(kXex);
  CHECK(launcher::CheckIso(iso, 0, h) == launcher::IsoCheck::kNoSpace);
  CHECK(launcher::CheckIso(iso, 256ull << 20, h) == launcher::IsoCheck::kNoSpace);  // payload + 256 MiB
  CHECK(launcher::CheckIso(iso, (256ull << 20) + kXex.size(), h) == launcher::IsoCheck::kOk);
}

TEST_CASE("Extraction: cancel mid-file stops the worker, reports progress, writes no marker", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_cancel");
  const auto iso = WriteIso(t.path, {{"default.xex", "XEX2"}, {"big.bin", std::string(96u << 20, 'z')}});
  const auto out = t.path / "out";
  launcher::Extraction ex;
  ex.Start(iso, out);
  for (int i = 0; i < 2000 && ex.done == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(ex.done > 0);
  CHECK(ex.CancelAndJoin() == 3);
  CHECK(ex.total > 0);
  CHECK_FALSE(ex.running());
  CHECK_FALSE(fs::exists(out / ".extract_ok"));
}

TEST_CASE("Extraction: an exception in the worker is logged and returns 1", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_throw");
  // Invalid UTF-8 in a disc name: the path conversion throws inside RunExtract.
  const auto iso = WriteIso(t.path, {{"default.xex", "XEX2"}, {"bad\xff\xfe.bin", "x"}});
  launcher::Extraction ex;
  ex.Start(iso, t.path / "out");
  for (int i = 0; i < 2000 && !ex.Finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(ex.Finished());
  CHECK(ex.result.get() == 1);
  CHECK(ex.LastLogLine().rfind("error: unexpected failure: ", 0) == 0);
  CHECK_FALSE(fs::exists(t.path / "out" / ".extract_ok"));
}

TEST_CASE("Extraction: runs to completion and writes the marker", "[game_setup]") {
  gow2_test::TempDir t("gow2_setup_full");
  const auto iso = WriteIso(t.path, {{"default.xex", "XEX2"}});
  launcher::Extraction ex;
  ex.Start(iso, t.path / "out");
  for (int i = 0; i < 2000 && !ex.Finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(ex.Finished());
  CHECK(ex.result.get() == 0);
  CHECK(fs::exists(t.path / "out" / ".extract_ok"));
}
