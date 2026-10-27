// Settings from gow2.toml: defaults, valid keys, errors with key and line.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "settings.h"
#include "temp_dir.h"

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {
fs::path Write(const fs::path& dir, const std::string& text) {
  const fs::path p = dir / "gow2.toml";
  std::ofstream(p, std::ios::binary) << text;
  return p;
}
}  // namespace

TEST_CASE("settings: a missing file leaves the defaults", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(dir.path / "doesnotexist.toml", s));
  CHECK(s.video.mode == settings::WindowMode::kWindowed);
  CHECK(s.video.width == 0);
  CHECK(s.video.vsync);
  CHECK(s.video.resizable);
  CHECK(s.gpu_cvars.empty());
}

TEST_CASE("settings: all the keys", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  const auto err = settings::LoadFile(Write(dir.path, "[video]\n"
                                                      "mode = \"borderless\"\n"
                                                      "width = 1920\n"
                                                      "height = 1080\n"
                                                      "scaling = \"stretch\"\n"
                                                      "vsync = false\n"
                                                      "resizable = false\n"
                                                      "[gpu]\n"
                                                      "cvars = { draw_resolution_scale_x = \"2\" }\n"),
                                      s);
  REQUIRE_FALSE(err);
  CHECK(s.video.mode == settings::WindowMode::kBorderless);
  CHECK(s.video.width == 1920);
  CHECK(s.video.height == 1080);
  CHECK(s.video.scaling == settings::Scaling::kStretch);
  CHECK_FALSE(s.video.vsync);
  CHECK_FALSE(s.video.resizable);
  REQUIRE(s.gpu_cvars.size() == 1);
  CHECK(s.gpu_cvars[0].first == "draw_resolution_scale_x");
  CHECK(s.gpu_cvars[0].second == "2");
}

TEST_CASE("settings: BOM and CRLF", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(
      Write(dir.path, "\xEF\xBB\xBF[video]\r\nmode = \"fullscreen\"\r\nwidth = 800\r\n"), s));
  CHECK(s.video.mode == settings::WindowMode::kFullscreen);
  CHECK(s.video.width == 800);
}

TEST_CASE("settings: errors quote the key", "[settings]") {
  gow2_test::TempDir dir("settings");
  const auto fail = [&](const std::string& text) {
    settings::Settings s;
    const auto err = settings::LoadFile(Write(dir.path, text), s);
    REQUIRE(err);
    CHECK(s.video.width == 0);
    return *err;
  };
  CHECK_THAT(fail("[video]\nmode = \"Fullscreen\"\n"), ContainsSubstring("mode"));
  const std::string w = fail("[video]\nwidth = 100\n");
  CHECK_THAT(w, ContainsSubstring("width"));
  CHECK_THAT(w, ContainsSubstring("320"));
  CHECK_THAT(w, ContainsSubstring("gow2.toml:2"));
  CHECK_THAT(fail("[video]\nfoo = 1\n"), ContainsSubstring("foo"));
  CHECK_THAT(fail("[other]\na = 1\n"), ContainsSubstring("other"));
  CHECK_THAT(fail("[video\n"), ContainsSubstring("gow2.toml"));
}

TEST_CASE("settings: ParseResolution", "[settings]") {
  uint32_t w = 0, h = 0;
  REQUIRE(settings::ParseResolution("1920x1080", w, h));
  CHECK(w == 1920);
  CHECK(h == 1080);
  CHECK_FALSE(settings::ParseResolution("1920*1080", w, h));
  CHECK_FALSE(settings::ParseResolution("0x0", w, h));
  CHECK_FALSE(settings::ParseResolution("8000x100", w, h));
  CHECK_FALSE(settings::ParseResolution("10x10", w, h));
}

TEST_CASE("settings: strict types", "[settings]") {
  gow2_test::TempDir dir("settings");
  for (const char* text : {"[video]\nvsync = 1\n", "[video]\nwidth = 1920.0\n"}) {
    settings::Settings s;
    const auto err = settings::LoadFile(Write(dir.path, text), s);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring(std::string(text).find("vsync") != std::string::npos ? "vsync" : "width"));
  }
}

TEST_CASE("settings: a folder in place of the file", "[settings]") {
  gow2_test::TempDir dir("settings");
  fs::create_directories(dir.path / "d.toml");
  settings::Settings s;
  const auto err = settings::LoadFile(dir.path / "d.toml", s);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("cannot read"));
}

TEST_CASE("settings: post-processing defaults", "[settings]") {
  const settings::Settings s;
  CHECK(s.video.antialiasing == settings::Antialiasing::kOff);
  CHECK_FALSE(s.video.sharpening);
  CHECK(s.video.sharpness == 0.0f);
  CHECK(s.video.internal_resolution == 1);
  CHECK(s.video.enhanced_shaders);
}

TEST_CASE("settings: valid post-processing keys", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(
      Write(dir.path, "[video]\nantialiasing = \"fxaa_extreme\"\nsharpening = true\nsharpness = 0.5\ninternal_resolution = 3\n"),
      s));
  CHECK(s.video.antialiasing == settings::Antialiasing::kFxaaExtreme);
  CHECK(s.video.sharpening);
  CHECK(s.video.sharpness == 0.5f);
  CHECK(s.video.internal_resolution == 3);
  settings::Settings t;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\nsharpness = 1\n"), t));
  CHECK(t.video.sharpness == 1.0f);
  settings::Settings u;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\nenhanced_shaders = false\n"), u));
  CHECK_FALSE(u.video.enhanced_shaders);
}

TEST_CASE("settings: invalid post-processing keys", "[settings]") {
  gow2_test::TempDir dir("settings");
  const std::pair<const char*, const char*> cases[] = {
      {"antialiasing = \"taa\"", "allowed values: \"off\", \"fxaa\", \"fxaa_extreme\""},
      {"sharpness = 1.5", "out of range 0.0..1.0"},
      {"sharpness = \"high\"", "expected a number"},
      {"internal_resolution = 4", "out of range 1..3"},
      {"internal_resolution = 0", "out of range 1..3"},
      {"sharpening = 1", "expected true or false"},
      {"enhanced_shaders = 3", "expected true or false"},
  };
  for (const auto& [line, why] : cases) {
    settings::Settings s;
    const auto err = settings::LoadFile(Write(dir.path, std::string("[video]\n") + line + "\n"), s);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring(std::string(line).substr(0, std::string(line).find(' '))));
    CHECK_THAT(*err, ContainsSubstring(why));
    CHECK(s.video.internal_resolution == 1);  // unchanged
    CHECK(s.video.antialiasing == settings::Antialiasing::kOff);
    CHECK(s.video.enhanced_shaders);
  }
  CHECK_FALSE(settings::ParseAntialiasing("FXAA"));
}

TEST_CASE("settings: anisotropic filtering", "[settings]") {
  CHECK(settings::Settings{}.video.anisotropic == 16);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\nanisotropic = 8\n"), s));
  CHECK(s.video.anisotropic == 8);
  settings::Settings t;
  const auto err = settings::LoadFile(Write(dir.path, "[video]\nanisotropic = 3\n"), t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("anisotropic"));
  CHECK_THAT(*err, ContainsSubstring("allowed values: 0, 2, 4, 8, 16"));
  CHECK(t.video.anisotropic == 16);  // unchanged
  const auto big = settings::LoadFile(Write(dir.path, "[video]\nanisotropic = 20\n"), t);
  REQUIRE(big);
  CHECK_THAT(*big, ContainsSubstring("allowed values: 0, 2, 4, 8, 16"));
  settings::Settings a;
  a.video.anisotropic = 4;
  const fs::path p = dir.path / "gow2.toml";
  REQUIRE_FALSE(settings::SaveFile(p, a));
  settings::Settings back;
  REQUIRE_FALSE(settings::LoadFile(p, back));
  CHECK(back.video.anisotropic == 4);
  CHECK(settings::AnisotropicOverride(0) == -1);
  CHECK(settings::AnisotropicOverride(2) == 2);
  CHECK(settings::AnisotropicOverride(4) == 3);
  CHECK(settings::AnisotropicOverride(8) == 4);
  CHECK(settings::AnisotropicOverride(16) == 5);
  CHECK(settings::AnisotropicOverride(3) == -1);
}

TEST_CASE("settings: AnisotropicOverrideName", "[settings]") {
  CHECK(settings::AnisotropicOverrideName(-1) == "game default");
  CHECK(settings::AnisotropicOverrideName(6) == "game default");
  CHECK(settings::AnisotropicOverrideName(0) == "off");
  CHECK(settings::AnisotropicOverrideName(1) == "1x");
  CHECK(settings::AnisotropicOverrideName(2) == "2x");
  CHECK(settings::AnisotropicOverrideName(3) == "4x");
  CHECK(settings::AnisotropicOverrideName(4) == "8x");
  CHECK(settings::AnisotropicOverrideName(5) == "16x");
}

TEST_CASE("settings: upscaler", "[settings]") {
  CHECK(settings::Settings{}.video.upscaler == settings::Upscaler::kOff);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\nupscaler = \"fsr\"\n"), s));
  CHECK(s.video.upscaler == settings::Upscaler::kFsr);
  settings::Settings t;
  const auto err = settings::LoadFile(Write(dir.path, "[video]\nupscaler = \"FSR\"\n"), t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("upscaler"));
  CHECK_THAT(*err, ContainsSubstring("allowed values: \"off\", \"fsr\", \"dlss\""));
  CHECK(t.video.upscaler == settings::Upscaler::kOff);
  CHECK(settings::ParseUpscaler("dlss") == settings::Upscaler::kDlss);
  CHECK(settings::ParseUpscaler("off") == settings::Upscaler::kOff);
  CHECK_FALSE(settings::ParseUpscaler(""));
}

TEST_CASE("settings: Serialize and LoadFile round-trip", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings all;
  all.video.mode = settings::WindowMode::kFullscreen;
  all.video.width = 2560;
  all.video.height = 1440;
  all.video.scaling = settings::Scaling::kStretch;
  all.video.vsync = false;
  all.video.resizable = false;
  all.video.antialiasing = settings::Antialiasing::kFxaaExtreme;
  all.video.sharpening = true;
  all.video.sharpness = 0.537f;  // slider: values with more than two decimals
  all.video.internal_resolution = 2;
  all.video.enhanced_shaders = false;
  all.video.upscaler = settings::Upscaler::kFsr;
  all.video.frame_generation = settings::FrameGeneration::kDlss;
  all.video.aspect = settings::Aspect::k16x10;
  all.video.hdr = true;
  all.video.hdr_peak_nits = 1500;
  all.video.hdr_paper_white_nits = 250;
  all.video.ui_scale = 125;
  all.video.dlss_preset = settings::DlssPreset::kM;
  all.language = "it";
  all.gpu_cvars = {{"a", "1"}, {"b", "x"}, {"c", "quote \" and backslash \\ and ü"}};  // TOML escapes
  std::vector<settings::Settings> cases{settings::Settings{}, all};
  for (const auto q : {settings::ShadowQuality::kLow, settings::ShadowQuality::kOriginal, settings::ShadowQuality::kHigh})
    for (const bool dyn : {true, false}) {
      settings::Settings e;
      e.engine = {q, dyn};
      cases.push_back(e);
    }
  for (const settings::Settings& s : cases) {
    const fs::path p = dir.path / "gow2.toml";
    REQUIRE_FALSE(settings::SaveFile(p, s));
    settings::Settings back;
    const auto err = settings::LoadFile(p, back);
    INFO(settings::Serialize(s));
    REQUIRE_FALSE(err);
    CHECK(back == s);
  }
}

TEST_CASE("settings: [game] language", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[game]\nlanguage = \"zh-cn\"\n"), s));
  CHECK(s.language == "zh-cn");
  settings::Settings t;
  auto err = settings::LoadFile(Write(dir.path, "[game]\nlanguage = \"xx\"\n"), t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("language") && ContainsSubstring("allowed values"));
  CHECK(t.language.empty());
  err = settings::LoadFile(Write(dir.path, "[game]\nfoo = 1\n"), t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("unknown key in [game]"));
}

TEST_CASE("settings: frame_generation", "[settings]") {
  using FG = settings::FrameGeneration;
  CHECK(settings::Settings{}.video.frame_generation == FG::kOff);
  gow2_test::TempDir dir("settings");
  const auto load = [&](const char* line, settings::Settings& t) {
    return settings::LoadFile(Write(dir.path, std::string("[video]\n") + line + "\n"), t);
  };
  for (const auto& [line, want] : {std::pair<const char*, FG>{"frame_generation = \"dlss\"", FG::kDlss},
                                   {"frame_generation = \"fsr\"", FG::kFsr},
                                   {"frame_generation = true", FG::kFsr},
                                   {"frame_generation = false", FG::kOff}}) {
    settings::Settings s;
    REQUIRE_FALSE(load(line, s));
    CHECK(s.video.frame_generation == want);
  }
  for (const char* bad : {"frame_generation = \"on\"", "frame_generation = \"DLSS\"", "frame_generation = 1"}) {
    settings::Settings t;
    const auto err = load(bad, t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("frame_generation"));
  }
  for (const FG f : {FG::kOff, FG::kFsr, FG::kDlss}) {
    CHECK(settings::ParseFrameGeneration(settings::FrameGenerationName(f)) == f);
    settings::Settings s, back;
    s.video.frame_generation = f;
    const fs::path p = dir.path / "rt.toml";
    REQUIRE_FALSE(settings::SaveFile(p, s));
    REQUIRE_FALSE(settings::LoadFile(p, back));
    CHECK(back == s);
  }
  CHECK_THAT(settings::Serialize(settings::Settings{}), ContainsSubstring("frame_generation = \"off\""));
}

TEST_CASE("settings: hdr", "[settings]") {
  const settings::Settings d;
  CHECK_FALSE(d.video.hdr);
  CHECK(d.video.hdr_peak_nits == 0);
  CHECK(d.video.hdr_paper_white_nits == 0);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(
      Write(dir.path, "[video]\nhdr = true\nhdr_peak_nits = 1000\nhdr_paper_white_nits = 203\n"), s));
  CHECK(s.video.hdr);
  CHECK(s.video.hdr_peak_nits == 1000);
  CHECK(s.video.hdr_paper_white_nits == 203);
  const auto load = [&](const char* line) {
    settings::Settings t;
    return settings::LoadFile(Write(dir.path, std::string("[video]\n") + line + "\n"), t);
  };
  auto err = load("hdr = \"on\"");
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("hdr"));
  for (const char* bad : {"hdr_peak_nits = 50", "hdr_peak_nits = 20000"}) {
    err = load(bad);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("0 (auto) or 100-10000"));
  }
  err = load("hdr_paper_white_nits = 79");
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("0 (auto) or 80-1000"));
  CHECK_FALSE(load("hdr_paper_white_nits = 0"));
  const std::string t = settings::Serialize(d);
  const auto fg = t.find("frame_generation = \"off\"");
  REQUIRE(fg != std::string::npos);
  CHECK(t.find("hdr = false", fg) != std::string::npos);
  CHECK(t.find("hdr_peak_nits = 0", fg) != std::string::npos);
  CHECK(t.find("hdr_paper_white_nits = 0", fg) != std::string::npos);
}

TEST_CASE("settings: dlss_preset", "[settings]") {
  using settings::DlssPreset;
  CHECK(settings::Settings{}.video.dlss_preset == DlssPreset::kDefault);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\ndlss_preset = \"m\"\n"), s));
  CHECK(s.video.dlss_preset == DlssPreset::kM);
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\ndlss_preset = \"k\"\n"), s));
  CHECK(s.video.dlss_preset == DlssPreset::kK);
  for (const char* bad : {"dlss_preset = \"a\"", "dlss_preset = \"b\"", "dlss_preset = \"g\"", "dlss_preset = 11",
                          "dlss_preset = \"M\""}) {
    settings::Settings t;
    const auto err = settings::LoadFile(Write(dir.path, std::string("[video]\n") + bad + "\n"), t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("dlss_preset"));
  }
  for (const DlssPreset p : {DlssPreset::kDefault, DlssPreset::kE, DlssPreset::kF, DlssPreset::kJ, DlssPreset::kK,
                             DlssPreset::kL, DlssPreset::kM}) {
    REQUIRE(settings::ParseDlssPreset(settings::DlssPresetName(p)));
    CHECK(*settings::ParseDlssPreset(settings::DlssPresetName(p)) == p);
  }
  CHECK(settings::ParseDlssPreset("j") == DlssPreset::kJ);
  CHECK_THAT(settings::Serialize(settings::Settings{}), ContainsSubstring("dlss_preset = \"default\""));
}

TEST_CASE("settings: dlss_mode", "[settings]") {
  using settings::DlssMode;
  CHECK(settings::Settings{}.video.dlss_mode == DlssMode::kAuto);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\ndlss_mode = \"ultra_performance\"\n"), s));
  CHECK(s.video.dlss_mode == DlssMode::kUltraPerformance);
  for (const char* bad : {"dlss_mode = \"Auto\"", "dlss_mode = \"ultra\"", "dlss_mode = 1"}) {
    settings::Settings t;
    const auto err = settings::LoadFile(Write(dir.path, std::string("[video]\n") + bad + "\n"), t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("dlss_mode"));
  }
  for (const DlssMode m : {DlssMode::kAuto, DlssMode::kDlaa, DlssMode::kQuality, DlssMode::kBalanced,
                           DlssMode::kPerformance, DlssMode::kUltraPerformance}) {
    REQUIRE(settings::ParseDlssMode(settings::DlssModeName(m)));
    CHECK(*settings::ParseDlssMode(settings::DlssModeName(m)) == m);
  }
  CHECK_THAT(settings::Serialize(settings::Settings{}), ContainsSubstring("dlss_mode = \"auto\""));
}

TEST_CASE("settings: DLSS mode -> internal resolution", "[settings]") {
  using settings::DlssMode;
  using settings::InternalResolutionForDlssMode;
  using std::nullopt;
  // 1440p
  CHECK(InternalResolutionForDlssMode(DlssMode::kPerformance, 2560) == 1u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kDlaa, 2560) == 2u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kQuality, 2560) == nullopt);
  CHECK(InternalResolutionForDlssMode(DlssMode::kBalanced, 2560) == nullopt);
  CHECK(InternalResolutionForDlssMode(DlssMode::kUltraPerformance, 2560) == nullopt);
  // 2160p
  CHECK(InternalResolutionForDlssMode(DlssMode::kUltraPerformance, 3840) == 1u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kQuality, 3840) == 2u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kDlaa, 3840) == 3u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kPerformance, 3840) == nullopt);
  CHECK(InternalResolutionForDlssMode(DlssMode::kBalanced, 3840) == nullopt);
  // 1080p
  CHECK(InternalResolutionForDlssMode(DlssMode::kQuality, 1920) == 1u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kDlaa, 1920) == 2u);
  // 4320p
  CHECK(InternalResolutionForDlssMode(DlssMode::kUltraPerformance, 7680) == 1u);
  CHECK(InternalResolutionForDlssMode(DlssMode::kPerformance, 7680) == 3u);
  for (const uint32_t w : {1920u, 2560u, 3840u, 7680u}) CHECK(InternalResolutionForDlssMode(DlssMode::kAuto, w) == nullopt);

  using settings::OutputSizeFor;
  using P = std::pair<uint32_t, uint32_t>;
  settings::Video v;
  v.mode = settings::WindowMode::kWindowed;
  v.width = 1600, v.height = 900;
  CHECK(OutputSizeFor(v, 3840, 2160) == P{1600, 900});  // small window on a 4K monitor: the window counts
  v.width = 0, v.height = 0;
  CHECK(OutputSizeFor(v, 3840, 2160) == P{1280, 720});
  v.mode = settings::WindowMode::kBorderless;
  CHECK(OutputSizeFor(v, 3840, 2160) == P{3840, 2160});
  v.mode = settings::WindowMode::kFullscreen;
  CHECK(OutputSizeFor(v, 3840, 2160) == P{3840, 2160});
  v.width = 2560, v.height = 1440;
  CHECK(OutputSizeFor(v, 3840, 2160) == P{2560, 1440});

  using settings::ImageWidthFor;
  settings::Video f;  // fit, 16:9
  CHECK(ImageWidthFor(f, 2560, 1440) == 2560u);
  CHECK(ImageWidthFor(f, 3440, 1440) == 2560u);
  CHECK(settings::ImageHeightFor(f, 3440, 1440) == 1440u);
  CHECK(settings::ImageHeightFor(f, 1920, 1200) == 1080u);  // 16:9 on 16:10: bars above and below
  f.aspect = settings::Aspect::k21x9;
  CHECK(ImageWidthFor(f, 3440, 1440) == 3351u);
  CHECK(settings::ImageHeightFor(f, 3440, 1440) == 1440u);
  f.scaling = settings::Scaling::kStretch;
  CHECK(ImageWidthFor(f, 3440, 1440) == 3440u);
  CHECK(settings::ImageHeightFor(f, 3440, 1440) == 1440u);

  using settings::EffectiveInternalResolution;
  settings::Video e;
  e.internal_resolution = 1;
  e.upscaler = settings::Upscaler::kDlss;
  e.dlss_mode = DlssMode::kDlaa;
  CHECK(EffectiveInternalResolution(e, 2560) == 2u);  // the mode wins over internal_resolution
  e.upscaler = settings::Upscaler::kFsr;
  CHECK(EffectiveInternalResolution(e, 2560) == 1u);  // with FSR the DLSS mode is ignored
  e.upscaler = settings::Upscaler::kOff;
  CHECK(EffectiveInternalResolution(e, 2560) == 1u);
  e.upscaler = settings::Upscaler::kDlss;
  e.dlss_mode = DlssMode::kQuality;
  e.internal_resolution = 3;
  CHECK(EffectiveInternalResolution(e, 2560) == 3u);  // unreachable: the file's value is used
  e.dlss_mode = DlssMode::kAuto;
  CHECK(EffectiveInternalResolution(e, 2560) == 3u);
}

TEST_CASE("old_dlss_nr_keys_are_ignored", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(
      Write(dir.path, "[video]\nupscaler = \"dlss\"\ndlss_nr = true\ndlss_nr_intensity = 1.5\ndlss_nr_style = \"natural\"\n"),
      s));
  CHECK(s.video.upscaler == settings::Upscaler::kDlss);
  CHECK_THAT(settings::Serialize(s), !ContainsSubstring("dlss_nr"));
}

TEST_CASE("settings: aspect", "[settings]") {
  using settings::Aspect;
  CHECK(settings::Settings{}.video.aspect == Aspect::k16x9);
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\naspect = \"21:9\"\n"), s));
  CHECK(s.video.aspect == Aspect::k21x9);
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\naspect = \"16:10\"\n"), s));
  CHECK(s.video.aspect == Aspect::k16x10);
  for (const char* bad : {"aspect = \"32:9\"", "aspect = 21"}) {
    settings::Settings t;
    const auto err = settings::LoadFile(Write(dir.path, std::string("[video]\n") + bad + "\n"), t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("aspect"));
  }
  for (const Aspect a : {Aspect::k16x9, Aspect::k21x9, Aspect::k16x10}) {
    REQUIRE(settings::ParseAspect(settings::AspectName(a)));
    CHECK(*settings::ParseAspect(settings::AspectName(a)) == a);
  }
  CHECK_FALSE(settings::ParseAspect("21/9"));
  CHECK_THAT(settings::Serialize(settings::Settings{}), ContainsSubstring("aspect = \"16:9\""));
}

TEST_CASE("settings: frame_rate and vrr", "[settings]") {
  using settings::FrameRate;
  using settings::Vrr;
  CHECK(settings::Settings{}.frame_rate == FrameRate::k30);
  CHECK(settings::Settings{}.video.vrr == Vrr::kAuto);
  gow2_test::TempDir dir("settings");
  const auto load = [&](const char* text, settings::Settings& s) { return settings::LoadFile(Write(dir.path, text), s); };
  settings::Settings s;
  REQUIRE_FALSE(load("[game]\nframe_rate = 144\n", s));
  CHECK(s.frame_rate == FrameRate::k144);
  REQUIRE_FALSE(load("[game]\nframe_rate = \"monitor\"\n", s));
  CHECK(s.frame_rate == FrameRate::kMonitor);
  REQUIRE_FALSE(load("[game]\nframe_rate = \"unlimited\"\n", s));
  CHECK(s.frame_rate == FrameRate::kUnlimited);
  for (const char* bad : {"[game]\nframe_rate = 90\n", "[game]\nframe_rate = \"60\"\n"}) {
    settings::Settings t;
    const auto err = load(bad, t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("frame_rate"));
    CHECK_THAT(*err, ContainsSubstring("allowed values: 30, 60, 120, 144, \"monitor\", \"unlimited\""));
    CHECK(t.frame_rate == FrameRate::k30);  // unchanged
  }
  REQUIRE_FALSE(load("[video]\nvrr = \"on\"\n", s));
  CHECK(s.video.vrr == Vrr::kOn);
  settings::Settings t;
  const auto err = load("[video]\nvrr = \"yes\"\n", t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("allowed values: \"auto\", \"on\", \"off\""));

  const fs::path p = dir.path / "gow2.toml";
  for (const std::string lang : {"", "it"})
    for (const FrameRate fr : {FrameRate::k30, FrameRate::k60, FrameRate::k120, FrameRate::k144, FrameRate::kMonitor,
                               FrameRate::kUnlimited})
      for (const Vrr vrr : {Vrr::kAuto, Vrr::kOn, Vrr::kOff}) {
        settings::Settings a;
        a.language = lang;
        a.frame_rate = fr;
        a.video.vrr = vrr;
        REQUIRE_FALSE(settings::SaveFile(p, a));
        settings::Settings back;
        REQUIRE_FALSE(settings::LoadFile(p, back));
        INFO(settings::Serialize(a));
        CHECK(back == a);
      }

  CHECK(settings::ParseFrameRate("30") == FrameRate::k30);
  CHECK(settings::ParseFrameRate("60") == FrameRate::k60);
  CHECK(settings::ParseFrameRate("120") == FrameRate::k120);
  CHECK(settings::ParseFrameRate("144") == FrameRate::k144);
  CHECK(settings::ParseFrameRate("monitor") == FrameRate::kMonitor);
  CHECK(settings::ParseFrameRate("unlimited") == FrameRate::kUnlimited);
  CHECK_FALSE(settings::ParseFrameRate("Unlimited"));
  CHECK_FALSE(settings::ParseFrameRate("90"));
  CHECK(settings::FrameRateName(FrameRate::k30) == "30");
  CHECK(settings::FrameRateName(FrameRate::k144) == "144");
  CHECK(settings::FrameRateName(FrameRate::kMonitor) == "monitor");
  CHECK(settings::FrameRateName(FrameRate::kUnlimited) == "unlimited");
  CHECK(settings::ParseVrr("auto") == Vrr::kAuto);
  CHECK(settings::ParseVrr("on") == Vrr::kOn);
  CHECK(settings::ParseVrr("off") == Vrr::kOff);
  CHECK_FALSE(settings::ParseVrr("Unlimited"));
  CHECK_FALSE(settings::ParseVrr("On"));
}

TEST_CASE("settings: [engine]", "[settings]") {
  using settings::ShadowQuality;
  const settings::Engine def = settings::Settings{}.engine;
  CHECK(def == settings::Engine{});
  CHECK(def.shadow_quality == ShadowQuality::kOriginal);
  CHECK(def.dynamic_shadows);
  gow2_test::TempDir dir("settings");
  const auto load = [&](const char* text, settings::Settings& s) { return settings::LoadFile(Write(dir.path, text), s); };
  settings::Settings s;
  // world_textures: option removed, the key in old files is ignored.
  REQUIRE_FALSE(load("[engine]\nshadow_quality = \"high\"\ndynamic_shadows = false\nworld_textures = \"high\"\n", s));
  CHECK(s.engine.shadow_quality == ShadowQuality::kHigh);
  CHECK_FALSE(s.engine.dynamic_shadows);
  REQUIRE_FALSE(load("[engine]\nshadow_quality = \"low\"\n", s));
  CHECK(s.engine.shadow_quality == ShadowQuality::kLow);

  const std::pair<const char*, const char*> bad[] = {
      {"shadow_quality = \"ultra\"", "allowed values: \"low\", \"original\", \"high\""},
      {"dynamic_shadows = \"no\"", "expected true or false"},
      {"foo = 1", "unknown key in [engine]"}};
  for (const auto& [line, why] : bad) {
    settings::Settings t;
    const auto err = load((std::string("[engine]\n") + line + "\n").c_str(), t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring(why));
    CHECK(t == settings::Settings{});  // unchanged
  }

  const std::string text = settings::Serialize(settings::Settings{});
  CHECK_THAT(text, ContainsSubstring("[engine]"));
  CHECK_THAT(text, ContainsSubstring("shadow_quality = \"original\""));
  CHECK_THAT(text, ContainsSubstring("dynamic_shadows = true"));
  CHECK_THAT(text, !ContainsSubstring("world_textures"));

  CHECK(settings::ParseShadowQuality("low") == ShadowQuality::kLow);
  CHECK(settings::ParseShadowQuality("original") == ShadowQuality::kOriginal);
  CHECK(settings::ParseShadowQuality("high") == ShadowQuality::kHigh);
  CHECK_FALSE(settings::ParseShadowQuality("High"));
  CHECK(settings::ShadowQualityName(ShadowQuality::kLow) == "low");
  CHECK(settings::ShadowQualityName(ShadowQuality::kOriginal) == "original");
  CHECK(settings::ShadowQualityName(ShadowQuality::kHigh) == "high");
}

TEST_CASE("settings: SaveFile", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  s.video.width = 1920;
  s.video.height = 1080;
  const fs::path missing = dir.path / "missing" / "gow2.toml";
  CHECK(settings::SaveFile(missing, s));
  CHECK_FALSE(fs::exists(missing.parent_path()));
  const fs::path p = Write(dir.path, "# old\n[video]\nmode = \"borderless\"\n");
  REQUIRE_FALSE(settings::SaveFile(p, s));
  settings::Settings back;
  REQUIRE_FALSE(settings::LoadFile(p, back));
  CHECK(back == s);
  for (const auto& e : fs::directory_iterator(dir.path)) CHECK(e.path().filename() == "gow2.toml");
}

TEST_CASE("settings: [patches]", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[patches]\ndisable_bloom = true\nshow_fps = false\n"), s));
  CHECK(s.patches == std::set<std::string>{"disable_bloom"});
  settings::Settings t;
  const auto err = settings::LoadFile(Write(dir.path, "[patches]\nfoo = true\n"), t);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("unknown key in [patches]") && ContainsSubstring("disable_motion_blur"));
  s.patches = {"show_fps", "disable_lens_flares"};
  settings::Settings back;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, settings::Serialize(s)), back));
  CHECK(back == s);
}

TEST_CASE("settings: ui_scale", "[settings]") {
  CHECK(settings::Settings{}.video.ui_scale == 100);
  gow2_test::TempDir dir("settings");
  for (const uint32_t n : settings::kUiScales) {
    CAPTURE(n);
    settings::Settings s;
    REQUIRE_FALSE(settings::LoadFile(Write(dir.path, "[video]\nui_scale = " + std::to_string(n) + "\n"), s));
    CHECK(s.video.ui_scale == n);
    CHECK_THAT(settings::Serialize(s), ContainsSubstring("ui_scale = " + std::to_string(n) + "\n"));
    const fs::path p = dir.path / "roundtrip.toml";
    REQUIRE_FALSE(settings::SaveFile(p, s));
    settings::Settings back;
    REQUIRE_FALSE(settings::LoadFile(p, back));
    CHECK(back == s);
  }
  for (const char* bad : {"80", "0", "200"}) {
    CAPTURE(bad);
    settings::Settings t;
    const auto err = settings::LoadFile(Write(dir.path, std::string("[video]\nui_scale = ") + bad + "\n"), t);
    REQUIRE(err);
    CHECK_THAT(*err, ContainsSubstring("ui_scale"));
    CHECK_THAT(*err, ContainsSubstring("allowed values: 75, 90, 100, 110, 125, 150"));
    CHECK(t.video.ui_scale == 100);  // unchanged
  }
  settings::Settings u;
  const auto str = settings::LoadFile(Write(dir.path, "[video]\nui_scale = \"100\"\n"), u);
  REQUIRE(str);
  CHECK_THAT(*str, ContainsSubstring("ui_scale: expected an integer"));
}

TEST_CASE("settings: [debug] console", "[settings]") {
  gow2_test::TempDir dir("settings");
  settings::Settings def;
  CHECK_FALSE(def.console);
  CHECK_THAT(settings::Serialize(def), !ContainsSubstring("[debug]"));
  settings::Settings on;
  on.console = true;
  const std::string text = settings::Serialize(on);
  CHECK_THAT(text, ContainsSubstring("[debug]"));
  CHECK_THAT(text, ContainsSubstring("console = true"));
  settings::Settings back;
  REQUIRE_FALSE(settings::LoadFile(Write(dir.path, text.c_str()), back));
  CHECK(back == on);
  settings::Settings bad;
  const auto err = settings::LoadFile(Write(dir.path, "[debug]\nfoo = 1\n"), bad);
  REQUIRE(err);
  CHECK_THAT(*err, ContainsSubstring("unknown key in [debug]"));
}
