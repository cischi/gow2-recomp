// OptiScaler (runtime/launcher/optiscaler): hash, detection, ini, installation with fake zip and fetch.
// No download: the tests' fetch copies a zip created here with Windows' tar.exe.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_data.h"
#include "launcher/optiscaler.h"
#include "settings.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

void WriteFile(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << bytes;
}

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const fs::path kFakeOptiScaler = GOW2_FAKE_OPTISCALER_DLL;

// Minimal ini with all the keys of the default settings (the fork's "auto" values) and, in other sections,
// keys with the same names that must not be touched.
const std::string kIni =
    "[Upscalers]\nDx12Upscaler=auto\nEnabled=auto\n[FrameGen]\nEnabled=auto\nFGInput=auto\nFGOutput=auto\n"
    "[FSRFG]\nAllowAsync=auto\nEnabled=auto\n[MFGUnlock]\nXeFG=auto\nRTX40=auto\n[Menu]\nFGShortcutKey=auto\nEnabled=auto\n"
    "[DlssNr]\n; c\nEnabled=auto\nRunBeforeSR=auto\nPreset=auto\nReversibleMode=auto\nWorkingScale=auto\n"
    "SkinStructure=auto\nLocalTone=auto\nToggleKey=auto\nFinishedPicture=auto\n[HDR]\nEnabled=auto\nPreset=auto\nToggleKey=auto\n";
const std::string kPatchedIni =
    "[Upscalers]\nDx12Upscaler=dlss\nEnabled=auto\n[FrameGen]\nEnabled=true\nFGInput=upscaler\nFGOutput=dlssg\n"
    "[FSRFG]\nAllowAsync=true\nEnabled=auto\n[MFGUnlock]\nXeFG=auto\nRTX40=detect\n[Menu]\nFGShortcutKey=0x76\nEnabled=auto\n"
    "[DlssNr]\n; c\nEnabled=true\nRunBeforeSR=true\nPreset=2\nReversibleMode=7\nWorkingScale=0.75\n"
    "SkinStructure=0.75\nLocalTone=0.0\nToggleKey=0x75\nFinishedPicture=true\n[HDR]\nEnabled=auto\nPreset=auto\nToggleKey=auto\n";

// Zip like the fork's (in miniature), created with Windows' tar.exe in dir/pkg.zip.
fs::path MakeZip(const fs::path& dir) {
  const fs::path src = dir / "src";
  WriteFile(src / "OptiScaler.dll", "dll-from-zip");
  WriteFile(src / "OptiScaler.ini", kIni);
  WriteFile(src / "nvngx.dll_dlssnr.dll", "forwarder");
  WriteFile(src / "OptiScaler" / "libxess.dll", "xess");
  WriteFile(src / "OptiScaler" / "nvngx_dlss.dll", "nvidia");
  WriteFile(src / "OptiScaler" / "streamline" / "nvngx_dlssg.dll", "nvidia");
  WriteFile(src / "docs" / "x.md", "doc");
  const fs::path zip = dir / "pkg.zip";
  char sys[MAX_PATH];
  REQUIRE(GetSystemDirectoryA(sys, MAX_PATH) != 0);
  const std::string tar = std::string(sys) + "\\tar.exe";
  // cmd /c strips the first and the last quote: the whole line must be wrapped in more quotes.
  const std::string cmd = "\"\"" + tar + "\" -a -cf \"" + zip.string() + "\" -C \"" + src.string() +
                          "\" OptiScaler.dll OptiScaler.ini nvngx.dll_dlssnr.dll OptiScaler docs\"";
  REQUIRE(std::system(cmd.c_str()) == 0);
  return zip;
}

// Install's temporary folders left in %TEMP% (gow2_optiscaler_<pid>_<tick>).
int LeftoverInstallDirs() {
  const std::string prefix = "gow2_optiscaler_" + std::to_string(GetCurrentProcessId()) + "_";
  int n = 0;
  for (const auto& e : fs::directory_iterator(fs::temp_directory_path()))
    n += e.path().filename().string().starts_with(prefix);
  return n;
}

optiscaler::InstallConfig ConfigFor(const fs::path& zip) {
  optiscaler::InstallConfig cfg;
  cfg.url = L"https://example.invalid/pkg.zip";
  cfg.zip_sha256 = "";
  cfg.fetch = [zip](std::wstring_view, const fs::path& dest) -> std::optional<std::string> {
    fs::copy_file(zip, dest);
    return std::nullopt;
  };
  return cfg;
}

}  // namespace

TEST_CASE("sha256_of_known_bytes", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_sha");
  WriteFile(tmp.path / "abc", "abc");
  CHECK(game_data::Sha256File(tmp.path / "abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_FALSE(game_data::Sha256File(tmp.path / "missing").has_value());
}

TEST_CASE("detect_states", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_detect");
  CHECK(optiscaler::Detect(tmp.path) == optiscaler::State::kMissing);
  WriteFile(tmp.path / "dxgi.dll", "x");
  CHECK(optiscaler::Detect(tmp.path) == optiscaler::State::kForeign);
  fs::copy_file(kFakeOptiScaler, tmp.path / "dxgi.dll", fs::copy_options::overwrite_existing);
  CHECK(optiscaler::Detect(tmp.path) == optiscaler::State::kOther);
  const auto own = game_data::Sha256File(kFakeOptiScaler);
  REQUIRE(own.has_value());
  CHECK(optiscaler::Detect(tmp.path, *own) == optiscaler::State::kInstalled);
}

TEST_CASE("patch_ini_sets_presets_by_section", "[optiscaler]") {
  CHECK(optiscaler::PatchIni(kIni) == kPatchedIni);
}

TEST_CASE("patch_ini_keeps_crlf_and_spaced_keys", "[optiscaler]") {
  // OptiScaler rewrites "Key = value" when the user saves; CRLF lines stay CRLF.
  const std::string ini = "[Upscalers]\r\nDx12Upscaler = auto\r\n[FrameGen]\r\nEnabled = auto\r\nFGInput = auto\r\n"
                          "FGOutput = auto\r\n[FSRFG]\r\nAllowAsync = auto\r\n[MFGUnlock]\r\nRTX40 = auto\r\n[Menu]\r\nFGShortcutKey = auto\r\n"
                          "[DlssNr]\r\nEnabled = auto\r\nRunBeforeSR = auto\r\nPreset = auto\r\nReversibleMode = auto\r\n"
                          "WorkingScale = auto\r\nSkinStructure = auto\r\nLocalTone = auto\r\nToggleKey = auto\r\n"
                          "FinishedPicture = auto\r\n";
  const std::string out = optiscaler::PatchIni(ini);
  REQUIRE_FALSE(out.empty());
  CHECK_THAT(out, Catch::Matchers::ContainsSubstring("Dx12Upscaler = dlss\r\n"));
  CHECK_THAT(out, Catch::Matchers::ContainsSubstring("ToggleKey = 0x75\r\n"));
  CHECK_THAT(out, Catch::Matchers::ContainsSubstring("FinishedPicture = true\r\n"));
  CHECK_THAT(out, Catch::Matchers::ContainsSubstring("[FSRFG]\r\nAllowAsync = true\r\n"));
}

TEST_CASE("patch_ini_with_missing_key_is_empty", "[optiscaler]") {
  CHECK(optiscaler::PatchIni("[DlssNr]\nFoo=1\n").empty());
  CHECK(optiscaler::PatchIni("[HDR]\nEnabled=auto\n").empty());
  // A key exists only in another section: [HDR]'s ToggleKey does not count for [DlssNr].
  std::string ini = kIni;
  ini.erase(ini.find("ToggleKey=auto\nFinishedPicture"), std::string("ToggleKey=auto\n").size());
  CHECK(optiscaler::PatchIni(ini).empty());
}

TEST_CASE("nr_enabled_unless_explicitly_false", "[optiscaler]") {
  CHECK_FALSE(optiscaler::NrEnabled("[DlssNr]\nToggleKey = 0x75\nEnabled = false\nRunBeforeSR = true\n"));
  CHECK_FALSE(optiscaler::NrEnabled("[DlssNr]\r\nEnabled=false\r\n"));
  CHECK(optiscaler::NrEnabled("[DlssNr]\nEnabled = true\n"));
  CHECK(optiscaler::NrEnabled("[DlssNr]\nEnabled = auto\n"));
  CHECK(optiscaler::NrEnabled(""));
  // Another section's Enabled does not count.
  CHECK(optiscaler::NrEnabled("[FrameGen]\nEnabled = false\n[DlssNr]\nEnabled = true\n"));
  CHECK(optiscaler::NrEnabled("[DlssNr]\nEnabled = true\n[FrameGen]\nEnabled = false\n"));
}

TEST_CASE("install_removes_stale_temp_dirs", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_stale");
  const fs::path stale = fs::temp_directory_path() / "gow2_optiscaler_999999_42";
  WriteFile(stale / "pkg.zip", "x");
  const fs::path exe = tmp.path / "exe";
  fs::create_directories(exe);
  auto cfg = ConfigFor(tmp.path / "missing.zip");
  cfg.fetch = [](std::wstring_view, const fs::path&) -> std::optional<std::string> { return "network"; };
  CHECK(optiscaler::Install(exe, cfg).has_value());
  CHECK_FALSE(fs::exists(stale));
}

TEST_CASE("install_happy_path", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_happy");
  const fs::path zip = MakeZip(tmp.path);
  const fs::path exe = tmp.path / "exe";
  fs::create_directories(exe);
  auto cfg = ConfigFor(zip);
  const auto zip_hash = game_data::Sha256File(zip);
  REQUIRE(zip_hash.has_value());
  cfg.zip_sha256 = *zip_hash;
  cfg.dll_sha256 = "";

  CHECK(optiscaler::Install(exe, cfg).value_or("installed") == "installed");
  CHECK(ReadFile(exe / "dxgi.dll") == "dll-from-zip");
  CHECK(ReadFile(exe / "OptiScaler.ini") == kPatchedIni);
  CHECK_THAT(ReadFile(exe / "OptiScaler.ini"), Catch::Matchers::ContainsSubstring("FGOutput=dlssg"));
  CHECK(ReadFile(exe / "nvngx.dll_dlssnr.dll") == "forwarder");
  CHECK(ReadFile(exe / "OptiScaler" / "libxess.dll") == "xess");
  CHECK(ReadFile(exe / "OptiScaler" / "streamline" / "nvngx_dlssg.dll") == "nvidia");
  CHECK(ReadFile(exe / "OptiScaler" / "nvngx_dlss.dll") == "nvidia");
  CHECK_FALSE(fs::exists(exe / "docs"));
  CHECK_FALSE(fs::exists(exe / "OptiScaler.dll"));
  CHECK(LeftoverInstallDirs() == 0);
}

TEST_CASE("install_rejects_hash_mismatch", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_hash");
  const fs::path exe = tmp.path / "exe";
  fs::create_directories(exe);
  auto cfg = ConfigFor(MakeZip(tmp.path));
  cfg.zip_sha256 = "0000";
  const auto msg = optiscaler::Install(exe, cfg);
  REQUIRE(msg.has_value());
  CHECK_THAT(*msg, Catch::Matchers::ContainsSubstring("SHA-256"));
  CHECK(fs::is_empty(exe));
}

TEST_CASE("install_refuses_foreign_dxgi", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_foreign");
  const fs::path exe = tmp.path / "exe";
  WriteFile(exe / "dxgi.dll", "x");
  auto cfg = ConfigFor(MakeZip(tmp.path));
  const std::string zip_hash = game_data::Sha256File(tmp.path / "pkg.zip").value();  // zip_sha256 is a view
  cfg.zip_sha256 = zip_hash;
  CHECK(optiscaler::Install(exe, cfg).has_value());
  CHECK(ReadFile(exe / "dxgi.dll") == "x");
  CHECK(std::distance(fs::directory_iterator(exe), fs::directory_iterator()) == 1);
}

TEST_CASE("install_replaces_other_optiscaler", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_other");
  const fs::path exe = tmp.path / "exe";
  fs::create_directories(exe);
  fs::copy_file(kFakeOptiScaler, exe / "dxgi.dll");
  WriteFile(exe / "OptiScaler" / "old.txt", "old");
  auto cfg = ConfigFor(MakeZip(tmp.path));
  const std::string zip_hash = game_data::Sha256File(tmp.path / "pkg.zip").value();  // zip_sha256 is a view
  cfg.zip_sha256 = zip_hash;
  CHECK(optiscaler::Install(exe, cfg).value_or("installed") == "installed");
  CHECK(ReadFile(exe / "dxgi.dll") == "dll-from-zip");
  // The old OptiScaler's files that the zip lacks stay; the backup copies have already been deleted.
  CHECK(ReadFile(exe / "OptiScaler" / "old.txt") == "old");
  CHECK_FALSE(fs::exists(exe / ".gow2_optiscaler_old"));
}

TEST_CASE("install_replaces_loaded_optiscaler", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_loaded");
  const fs::path exe = tmp.path / "exe";
  fs::create_directories(exe);
  fs::copy_file(kFakeOptiScaler, exe / "dxgi.dll");
  auto cfg = ConfigFor(MakeZip(tmp.path));
  const std::string zip_hash = game_data::Sha256File(tmp.path / "pkg.zip").value();  // zip_sha256 is a view
  cfg.zip_sha256 = zip_hash;
  // Mapped as an image, like OptiScaler's dxgi.dll loaded in the game: it cannot be deleted, only renamed.
  HMODULE mapped = LoadLibraryExW((exe / "dxgi.dll").c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  REQUIRE(mapped != nullptr);
  CHECK(optiscaler::Install(exe, cfg).value_or("installed") == "installed");
  CHECK(ReadFile(exe / "dxgi.dll") == "dll-from-zip");
  CHECK(fs::exists(exe / ".gow2_optiscaler_old"));  // the old dxgi.dll is still mapped
  FreeLibrary(mapped);
  optiscaler::SweepOld(exe);
  CHECK_FALSE(fs::exists(exe / ".gow2_optiscaler_old"));
}

TEST_CASE("install_reports_fetch_failure", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_fetch");
  optiscaler::InstallConfig cfg;
  cfg.fetch = [](std::wstring_view, const fs::path&) -> std::optional<std::string> { return "network down"; };
  const auto msg = optiscaler::Install(tmp.path, cfg);
  REQUIRE(msg.has_value());
  CHECK_THAT(*msg, Catch::Matchers::ContainsSubstring("network down"));
  CHECK(fs::is_empty(tmp.path));
}

TEST_CASE("install_rolls_back_on_copy_failure", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_rollback");
  const fs::path exe = tmp.path / "exe";
  WriteFile(exe / "OptiScaler" / "libxess.dll", "locked");
  auto cfg = ConfigFor(MakeZip(tmp.path));
  const std::string zip_hash = game_data::Sha256File(tmp.path / "pkg.zip").value();  // zip_sha256 is a view
  cfg.zip_sha256 = zip_hash;
  // Opened without sharing: neither the backup copy nor the overwrite can succeed.
  HANDLE h = CreateFileW((exe / "OptiScaler" / "libxess.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  const auto msg = optiscaler::Install(exe, cfg);
  CloseHandle(h);
  REQUIRE(msg.has_value());
  CHECK_THAT(*msg, Catch::Matchers::ContainsSubstring("libxess.dll"));
  CHECK_FALSE(fs::exists(exe / "dxgi.dll"));
  CHECK_FALSE(fs::exists(exe / "OptiScaler.ini"));
  CHECK_FALSE(fs::exists(exe / "nvngx.dll_dlssnr.dll"));
  CHECK(ReadFile(exe / "OptiScaler" / "libxess.dll") == "locked");
  CHECK_FALSE(fs::exists(exe / "OptiScaler" / "libxess.dll.new"));
  CHECK_FALSE(fs::exists(exe / ".gow2_optiscaler_old"));
  CHECK(LeftoverInstallDirs() == 0);
}

TEST_CASE("install_cancelled_changes_nothing", "[optiscaler]") {
  gow2_test::TempDir tmp("gow2_optiscaler_cancel");
  const fs::path exe = tmp.path / "exe";
  WriteFile(exe / "gow2.exe", "exe");
  const fs::path zip = MakeZip(tmp.path);
  const std::string zip_hash = game_data::Sha256File(zip).value();  // zip_sha256 is a view
  auto cfg = ConfigFor(zip);
  cfg.zip_sha256 = zip_hash;
  cfg.dll_sha256 = "";
  const auto fetch = cfg.fetch;
  // Cancelled during the download (like closing the game): the zip has arrived but nothing is copied.
  cfg.fetch = [fetch](std::wstring_view url, const fs::path& dest) {
    const auto err = fetch(url, dest);
    optiscaler::CancelInstall();
    return err;
  };
  const auto msg = optiscaler::Install(exe, cfg);
  REQUIRE(msg.has_value());
  CHECK(*msg == "installation cancelled");
  CHECK(std::vector<fs::path>(fs::directory_iterator(exe), {}) == std::vector<fs::path>{exe / "gow2.exe"});
  CHECK(LeftoverInstallDirs() == 0);

  // The cancellation does not apply to later installations, even if requested when none is in progress.
  optiscaler::CancelInstall();
  cfg.fetch = fetch;
  CHECK(optiscaler::Install(exe, cfg).value_or("installed") == "installed");
  CHECK(ReadFile(exe / "dxgi.dll") == "dll-from-zip");
}

TEST_CASE("wait_install_copy_without_install_returns", "[optiscaler]") {
  optiscaler::WaitInstallCopy();  // no copy in progress: returns immediately (otherwise the test times out)
  optiscaler::WaitInstallCopy();
}

TEST_CASE("optiscaler_forces_fg_off") {
  using FG = settings::FrameGeneration;
  using S = optiscaler::State;
  CHECK(optiscaler::EffectiveFrameGeneration(FG::kDlss, S::kInstalled) == FG::kOff);
  CHECK(optiscaler::EffectiveFrameGeneration(FG::kFsr, S::kInstalled) == FG::kOff);
  CHECK(optiscaler::EffectiveFrameGeneration(FG::kFsr, S::kOther) == FG::kFsr);
  CHECK(optiscaler::EffectiveFrameGeneration(FG::kDlss, S::kMissing) == FG::kDlss);
  CHECK(optiscaler::EffectiveFrameGeneration(FG::kDlss, S::kForeign) == FG::kDlss);
}
