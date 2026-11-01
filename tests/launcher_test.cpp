// Launcher (runtime/launcher): model, ImGui panel without a backend, Win32 + D3D11 window.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gpu/d3d12_probe.h"
#include "launcher/launcher.h"
#include "launcher/imgui_style.h"
#include "launcher/launcher_model.h"
#include "launcher/launcher_ui.h"
#include "launcher/optiscaler.h"
#include "settings.h"
#include "temp_dir.h"

using Res = std::pair<uint32_t, uint32_t>;

TEST_CASE("launcher: UnavailableOptions is empty for GoW2", "[launcher]") {
  CHECK(launcher::UnavailableOptions().empty());
}

TEST_CASE("launcher: ResolutionChoices", "[launcher]") {
  const auto r = launcher::ResolutionChoices({{1920, 1080}, {640, 400}, {2560, 1440}, {1920, 1080}, {800, 600}}, 0, 0);
  CHECK(r == std::vector<Res>{{2560, 1440}, {1920, 1080}, {1280, 720}, {800, 600}});
  const auto empty = launcher::ResolutionChoices({}, 0, 0);
  CHECK(empty == std::vector<Res>{{1280, 720}});
  const auto cur = launcher::ResolutionChoices({{1920, 1080}}, 1366, 768);
  CHECK(cur == std::vector<Res>{{1920, 1080}, {1366, 768}, {1280, 720}});
  const auto known = launcher::ResolutionChoices({{1920, 1080}}, 1920, 1080);
  CHECK(known == std::vector<Res>{{1920, 1080}, {1280, 720}});
}

TEST_CASE("launcher: Languages follows kLanguageCodes", "[launcher]") {
  const auto langs = launcher::Languages();
  REQUIRE(langs.size() == settings::kLanguageCodes.size());
  for (size_t i = 0; i < langs.size(); ++i) {
    CHECK(langs[i].code == settings::kLanguageCodes[i]);
    CHECK_FALSE(langs[i].label.empty());
  }
}

TEST_CASE("launcher: DlssBridgePresent wants gow2_dlss.dll and nvngx_dlss.dll", "[launcher]") {
  gow2_test::TempDir dir("launcher_dlss");
  CHECK_FALSE(launcher::DlssBridgePresent(dir.path));
  std::ofstream(dir.path / "gow2_dlss.dll") << "x";
  CHECK_FALSE(launcher::DlssBridgePresent(dir.path));
  std::ofstream(dir.path / "nvngx_dlss.dll") << "x";
  CHECK(launcher::DlssBridgePresent(dir.path));
}

TEST_CASE("launcher: DlssDownloadAvailable needs the bridge and a missing NVIDIA DLL", "[launcher]") {
  gow2_test::TempDir dir("launcher_dlss_dl");
  CHECK_FALSE(launcher::DlssDownloadAvailable(dir.path));
  std::ofstream(dir.path / "gow2_dlss.dll") << "x";
  CHECK(launcher::DlssDownloadAvailable(dir.path));
  std::ofstream(dir.path / "nvngx_dlss.dll") << "x";
  CHECK(launcher::DlssDownloadAvailable(dir.path));
  std::ofstream(dir.path / "nvngx_dlssg.dll") << "x";
  CHECK_FALSE(launcher::DlssDownloadAvailable(dir.path));
}

TEST_CASE("launcher: FrameGenerationDllsPresent wants the loader and the FG DLL", "[launcher]") {
  gow2_test::TempDir dir("launcher_fg");
  CHECK_FALSE(launcher::FrameGenerationDllsPresent(dir.path));
  std::ofstream(dir.path / "amd_fidelityfx_framegeneration_dx12.dll") << "x";
  CHECK_FALSE(launcher::FrameGenerationDllsPresent(dir.path));
  std::ofstream(dir.path / "amd_fidelityfx_loader_dx12.dll") << "x";
  CHECK(launcher::FrameGenerationDllsPresent(dir.path));
}

TEST_CASE("launcher: DlssFgDllsPresent wants the bridge and nvngx_dlssg.dll", "[launcher]") {
  gow2_test::TempDir dir("launcher_dlssfg");
  CHECK_FALSE(launcher::DlssFgDllsPresent(dir.path));
  std::ofstream(dir.path / "gow2_dlss.dll") << "x";
  CHECK_FALSE(launcher::DlssFgDllsPresent(dir.path));
  std::ofstream(dir.path / "nvngx_dlssg.dll") << "x";
  CHECK(launcher::DlssFgDllsPresent(dir.path));
  std::filesystem::remove(dir.path / "gow2_dlss.dll");
  CHECK_FALSE(launcher::DlssFgDllsPresent(dir.path));
}

TEST_CASE("launcher: texts of the DLSS frame generation choice", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    CHECK(std::string(t.frame_generation_off) == (l == L::kEnglish ? "Off" : "Spenta"));
    const std::string tip = t.frame_generation_dlss_tip;
    for (const char* w : {"NVIDIA DLSS", "RTX 40", "NVIDIA App", "borderless"}) CHECK(tip.find(w) != std::string::npos);
    CHECK(tip.find(l == L::kEnglish ? "next start" : "prossimo avvio") != std::string::npos);
    const std::string un = t.frame_generation_dlss_unavailable;
    CHECK(un.find("gow2_dlss.dll") != std::string::npos);
    CHECK(un.find("nvngx_dlssg.dll") != std::string::npos);
  }
}

TEST_CASE("launcher: QueryHdrDisplay", "[launcher]") {
  const hdr::Display none = launcher::QueryHdrDisplay(nullptr);
  CHECK_FALSE(none.hdr);
  CHECK(none.max_nits == 0);
  CHECK(none.sdr_white_nits == 0);
  CHECK(none.name.empty());
  if (!gpu::FindHardwareAdapter()) SKIP("no hardware D3D12 adapter");
  const hdr::Display d = launcher::QueryHdrDisplay(GetDesktopWindow());
  if (d.name.empty()) SKIP("no active output");
  CHECK_FALSE(d.name.empty());
  CHECK(d.max_nits >= 0);
  CHECK(d.sdr_white_nits >= 0);
  if (!d.hdr) CHECK(d.sdr_white_nits == 0);
}

TEST_CASE("launcher: DrawSettings without input changes nothing", "[launcher]") {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini in the current folder
  io.DisplaySize = ImVec2(640, 600);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // atlas without a backend: no texture to upload
  for (auto mode : {settings::WindowMode::kWindowed, settings::WindowMode::kBorderless, settings::WindowMode::kFullscreen})
    for (auto up : {settings::Upscaler::kOff, settings::Upscaler::kFsr, settings::Upscaler::kDlss}) {
      launcher::LauncherState st;
      st.s.video.mode = mode;
      st.s.video.upscaler = up;
      st.dlss_available = up == settings::Upscaler::kDlss;
      st.s.video.frame_generation = up == settings::Upscaler::kDlss ? settings::FrameGeneration::kDlss
                                                                    : settings::FrameGeneration::kFsr;
      st.dlss_fg_available = false;
      st.s.video.hdr = true;
      st.s.video.hdr_peak_nits = 1000;
      st.s.video.hdr_paper_white_nits = 0;
      st.s.video.aspect = settings::Aspect::k21x9;
      st.s.language = up == settings::Upscaler::kFsr ? "en" : "it";
      st.s.frame_rate = settings::FrameRate::kMonitor;
      st.s.video.vrr = settings::Vrr::kOff;
      st.s.engine = {settings::ShadowQuality::kHigh, false};
      st.restart = {launcher::RestartField::kLanguage};
      st.resolutions = launcher::ResolutionChoices({{1920, 1080}}, 0, 0);
      st.error = "gow2.toml:3: mode: allowed values";
      st.notice = launcher::Notice::kInvalidFile;
      const auto before = st.s;
      for (int f = 0; f < 3; ++f) {
        ImGui::NewFrame();
        CHECK(launcher::DrawSettings(st) == launcher::Action::kNone);
        ImGui::Render();
      }
      CHECK(st.s == before);
    }
  ImGui::DestroyContext();
}

namespace {
std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

TEST_CASE("launcher: Run with Play writes the file", "[launcher]") {
  gow2_test::TempDir dir("launcher");
  const auto p = dir.path / "gow2.toml";
  std::string err;
  REQUIRE(launcher::Run(p, {3, launcher::Action::kPlay}, &err) == launcher::Result::kPlay);
  INFO(err);
  REQUIRE(std::filesystem::exists(p));
  settings::Settings s;
  REQUIRE_FALSE(settings::LoadFile(p, s));
  CHECK(s == settings::Settings{});
}

TEST_CASE("launcher: Run with Quit writes nothing", "[launcher]") {
  gow2_test::TempDir dir("launcher");
  const auto p = dir.path / "gow2.toml";
  REQUIRE(launcher::Run(p, {3, launcher::Action::kQuit}) == launcher::Result::kQuit);
  CHECK_FALSE(std::filesystem::exists(p));
}

TEST_CASE("launcher: invalid file", "[launcher]") {
  gow2_test::TempDir dir("launcher");
  const auto p = dir.path / "gow2.toml";
  const std::string bad = "# mine\n[video]\nmode = \"bogus\"\n";
  std::ofstream(p, std::ios::binary) << bad;
  REQUIRE(launcher::Run(p, {3, launcher::Action::kQuit}) == launcher::Result::kQuit);
  CHECK(ReadAll(p) == bad);
  REQUIRE(launcher::Run(p, {3, launcher::Action::kPlay}) == launcher::Result::kPlay);
  settings::Settings s;
  CHECK_FALSE(settings::LoadFile(p, s));
}

TEST_CASE("launcher: LanguageLabel", "[launcher]") {
  CHECK(launcher::LanguageLabel("") == "English");
  CHECK(launcher::LanguageLabel(std::string("it")) == "Italiano");
  CHECK(launcher::LanguageLabel("zh-cn") == "Chinese, simplified (zh-cn)");
}

TEST_CASE("launcher: Enter with no other input presses Play", "[launcher]") {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini in the current folder
  io.DisplaySize = ImVec2(640, 600);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  launcher::LauncherState st;
  launcher::Action got = launcher::Action::kNone;
  for (int f = 0; f < 8 && got == launcher::Action::kNone; ++f) {
    if (f == 3) io.AddKeyEvent(ImGuiKey_Enter, true);
    if (f == 4) io.AddKeyEvent(ImGuiKey_Enter, false);
    ImGui::NewFrame();
    got = launcher::DrawSettings(st);
    ImGui::Render();
  }
  CHECK(got == launcher::Action::kPlay);
  ImGui::DestroyContext();
}

// Clicks every few pixels down the left edge until DrawSettings returns an action; headless, no backend.
static launcher::Action ClickFirstButton(launcher::LauncherState& st) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640, 600);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  launcher::Action got = launcher::Action::kNone;
  const auto frame = [&] {
    ImGui::NewFrame();
    const auto a = launcher::DrawSettings(st);
    if (got == launcher::Action::kNone) got = a;
    ImGui::Render();
  };
  frame();
  frame();
  for (float y = 20; y < 200 && got == launcher::Action::kNone; y += 6) {
    io.AddMousePosEvent(40, y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
  }
  ImGui::DestroyContext();
  return got;
}

TEST_CASE("launcher: Game data section returns kChooseIso, then kCancelExtract while extracting", "[launcher]") {
  launcher::LauncherState st;
  st.data_ready = false;
  CHECK(ClickFirstButton(st) == launcher::Action::kChooseIso);
  launcher::LauncherState busy;
  busy.data_ready = false;
  busy.extracting = true;
  CHECK(ClickFirstButton(busy) == launcher::Action::kCancelExtract);
  launcher::LauncherState overlay;
  overlay.overlay = true;
  const auto a = ClickFirstButton(overlay);
  CHECK(a != launcher::Action::kChooseIso);
  CHECK(a != launcher::Action::kCancelExtract);
}

// The texts filled from the profile are, for gow2, exactly the ones written out before the placeholders.
TEST_CASE("launcher: gow2 texts with game name, edition and exe are unchanged", "[launcher]") {
  const auto& it = launcher::Text(launcher::UiLanguage::kItalian);
  const auto& en = launcher::Text(launcher::UiLanguage::kEnglish);
  const std::pair<const char*, const char*> expected[] = {
      {it.frame_generation_unavailable,
       "Non disponibile: manca amd_fidelityfx_framegeneration_dx12.dll accanto a gow2.exe"},
      {it.frame_generation_dlss_unavailable, "Non disponibile: mancano gow2_dlss.dll o nvngx_dlssg.dll accanto a gow2.exe"},
      {it.optiscaler_tip,
       "Scarica OptiScaler %s (fork F5, GPL-3) da GitHub e lo mette accanto a gow2.exe. Il rendering neurale richiede "
       "nvngx_dlssnr.dll, da procurarsi a parte. Il launcher si riavvia: le modifiche non salvate vanno perse."},
      {it.optiscaler_no_dlssnr,
       "Il rendering neurale richiede nvngx_dlssnr.dll accanto a gow2.exe (nvngx_dlssnr.dll non fornita dal progetto)."},
      {it.data_missing, "Dati del gioco mancanti: scegli la tua ISO di Gears of War 2 (PAL Europa)."},
      {it.iso_wrong_edition, "Edizione non supportata: serve l'ISO PAL Europa di Gears of War 2."},
      {it.extract_wrong_edition, "Edizione non supportata: i file estratti non corrispondono all'ISO PAL Europa."},
      {en.frame_generation_unavailable, "Not available: amd_fidelityfx_framegeneration_dx12.dll is missing next to gow2.exe"},
      {en.frame_generation_dlss_unavailable, "Not available: gow2_dlss.dll or nvngx_dlssg.dll is missing next to gow2.exe"},
      {en.optiscaler_tip,
       "Downloads OptiScaler %s (F5 fork, GPL-3) from GitHub and puts it next to gow2.exe. Neural rendering needs "
       "nvngx_dlssnr.dll, to be obtained separately. The launcher restarts: unsaved changes are lost."},
      {en.optiscaler_no_dlssnr,
       "Neural rendering needs nvngx_dlssnr.dll next to gow2.exe (nvngx_dlssnr.dll is not provided by the project)."},
      {en.data_missing, "Game data missing: choose your Gears of War 2 ISO (PAL Europe)."},
      {en.iso_wrong_edition, "Unsupported edition: the PAL Europe ISO of Gears of War 2 is required."},
      {en.extract_wrong_edition, "Unsupported edition: the extracted files do not match the PAL Europe ISO."},
  };
  for (const auto& [actual, want] : expected) CHECK(std::string(actual) == want);
}

TEST_CASE("launcher: FitScale keeps the window inside the work area", "[launcher]") {
  CHECK(launcher::FitScale(1.0f, 1200, 31) == 1.0f);
  CHECK(launcher::FitScale(1.5f, 2000, 46) == 1.5f);
  const float s = launcher::FitScale(1.25f, 728, 31);  // 1366x768 at 125%
  CHECK(s < 1.25f);
  CHECK(launcher::kWindowHeight * s + 31 <= 728);
  const float t = launcher::FitScale(1.75f, 1032, 54);  // 1920x1080 at 175%
  CHECK(launcher::kWindowHeight * t + 54 <= 1032);
  CHECK(launcher::FitScale(1.0f, 100, 31) > 0.0f);  // absurd area: never a zero scale
}

TEST_CASE("launcher: RestartNeeded and RestartText", "[launcher]") {
  using F = launcher::RestartField;
  const settings::Settings def;
  CHECK(launcher::RestartNeeded(def, def).empty());
  settings::Settings hot;
  hot.video.vsync = false;
  hot.video.scaling = settings::Scaling::kStretch;
  hot.video.sharpness = 0.5f;
  hot.video.mode = settings::WindowMode::kBorderless;
  hot.video.width = 1920;
  hot.video.anisotropic = 4;  // hot: the cvar changes on the next draw
  CHECK(launcher::RestartNeeded(def, hot).empty());
  settings::Settings lang;
  lang.language = "it";
  CHECK(launcher::RestartNeeded(def, lang) == std::vector<F>{F::kLanguage});
  settings::Settings many = lang;
  many.video.upscaler = settings::Upscaler::kFsr;
  many.video.internal_resolution = 2;
  const auto fields = launcher::RestartNeeded(def, many);
  CHECK(fields == std::vector<F>{F::kUpscaler, F::kInternalResolution, F::kLanguage});
  CHECK(launcher::RestartText(fields, launcher::UiLanguage::kItalian) == "Upscaler, Risoluzione interna, Lingua");
  CHECK(launcher::RestartText(fields, launcher::UiLanguage::kEnglish) == "Upscaler, Internal resolution, Language");
  settings::Settings con;
  con.console = true;
  CHECK(launcher::RestartNeeded(def, con) == std::vector<F>{F::kConsole});
  settings::Settings dlss;
  dlss.video.upscaler = settings::Upscaler::kDlss;
  CHECK(launcher::RestartNeeded(def, dlss) == std::vector<F>{F::kUpscaler});
  settings::Settings patch;
  patch.patches.insert("disable_bloom");
  CHECK(launcher::RestartNeeded(def, patch).empty());  // hot from the overlay
  settings::Settings aa;
  aa.video.antialiasing = settings::Antialiasing::kFxaa;
  CHECK(launcher::RestartNeeded(def, aa).empty());  // hot: SetDesiredSwapPostEffect
  settings::Settings rate;
  rate.frame_rate = settings::FrameRate::k60;
  CHECK(launcher::RestartNeeded(def, rate) == std::vector<F>{F::kFrameRate});
  CHECK(launcher::RestartText({F::kFrameRate}, launcher::UiLanguage::kItalian).find("Frame rate") != std::string::npos);
  settings::Settings vrr;
  vrr.video.vrr = settings::Vrr::kOn;  // hot: SetVideo
  CHECK(launcher::RestartNeeded(def, vrr).empty());
  settings::Settings sq, ds, all;
  sq.engine.shadow_quality = settings::ShadowQuality::kHigh;
  ds.engine.dynamic_shadows = false;
  all.engine = {settings::ShadowQuality::kLow, false};
  CHECK(launcher::RestartNeeded(def, sq) == std::vector<F>{F::kShadowQuality});
  CHECK(launcher::RestartNeeded(def, ds) == std::vector<F>{F::kDynamicShadows});
  CHECK(launcher::RestartNeeded(def, all) == std::vector<F>{F::kShadowQuality, F::kDynamicShadows});
  const std::vector<F> two{F::kShadowQuality, F::kDynamicShadows};
  CHECK(launcher::RestartText(two, launcher::UiLanguage::kItalian) == "Qualità ombre, Ombre dinamiche");
  CHECK(launcher::RestartText(two, launcher::UiLanguage::kEnglish) == "Shadow quality, Dynamic shadows");
}

TEST_CASE("launcher: RestartNeeded for the DLSS preset", "[launcher]") {
  using F = launcher::RestartField;
  const settings::Settings def;
  settings::Settings m;
  m.video.dlss_preset = settings::DlssPreset::kM;
  CHECK(launcher::RestartNeeded(def, m) == std::vector<F>{F::kDlssPreset});
  CHECK(launcher::RestartNeeded(m, m).empty());
  CHECK(launcher::RestartText({F::kDlssPreset}, launcher::UiLanguage::kItalian) == "Preset DLSS");
  CHECK(launcher::RestartText({F::kDlssPreset}, launcher::UiLanguage::kEnglish) == "DLSS preset");
}

TEST_CASE("launcher: DLSS mode, restart and labels", "[launcher]") {
  using F = launcher::RestartField;
  using M = settings::DlssMode;
  const settings::Settings def;
  settings::Settings m;
  m.video.dlss_mode = M::kPerformance;
  CHECK(launcher::RestartNeeded(def, m) == std::vector<F>{F::kDlssMode});
  CHECK(launcher::RestartText({F::kDlssMode}, launcher::UiLanguage::kItalian) == "Modalità DLSS");
  CHECK(launcher::RestartText({F::kDlssMode}, launcher::UiLanguage::kEnglish) == "DLSS mode");
  const auto it = launcher::UiLanguage::kItalian;
  CHECK(launcher::DlssModeLabel(M::kPerformance, 1u, settings::Aspect::k16x9, 2560, 1440, it) ==
        "Prestazioni - 1280x720 -> 2560x1440");
  CHECK(launcher::DlssModeLabel(M::kDlaa, 2u, settings::Aspect::k16x9, 2560, 1440, launcher::UiLanguage::kEnglish) ==
        "DLAA - 2560x1440 -> 2560x1440");
  CHECK(launcher::DlssModeLabel(M::kQuality, std::nullopt, settings::Aspect::k16x9, 2560, 1440, it) == "Qualità");
  // stretch: the image is the whole output, the height does not come from the aspect ratio
  CHECK(launcher::DlssModeLabel(M::kPerformance, 1u, settings::Aspect::k16x9, 3440, 1440, it) ==
        "Prestazioni - 1280x720 -> 3440x1440");
  CHECK(launcher::DlssModeLabel(M::kAuto, std::nullopt, settings::Aspect::k16x9, 2560, 1440, it) == "Automatica");
  CHECK(launcher::DlssModeLabel(M::kUltraPerformance, std::nullopt, settings::Aspect::k16x9, 2560, 1440,
                                launcher::UiLanguage::kEnglish) == "Ultra performance");
}

TEST_CASE("launcher: DlssNrRuntimePresent looks for nvngx_dlssnr.dll", "[launcher]") {
  gow2_test::TempDir dir("launcher_dlssnr");
  CHECK_FALSE(launcher::DlssNrRuntimePresent(dir.path));
  std::ofstream(dir.path / "nvngx_dlssnr.dll") << "x";
  CHECK(launcher::DlssNrRuntimePresent(dir.path));
}

TEST_CASE("launcher: RestartNeeded for frame generation", "[launcher]") {
  using F = launcher::RestartField;
  using FG = settings::FrameGeneration;
  const auto with = [](FG f) {
    settings::Settings s;
    s.video.frame_generation = f;
    return s;
  };
  const std::vector<F> fg{F::kFrameGeneration};
  CHECK(launcher::RestartNeeded(with(FG::kOff), with(FG::kFsr)) == fg);
  CHECK(launcher::RestartNeeded(with(FG::kOff), with(FG::kDlss)) == fg);
  CHECK(launcher::RestartNeeded(with(FG::kFsr), with(FG::kDlss)) == fg);
  CHECK(launcher::RestartNeeded(with(FG::kFsr), with(FG::kOff)).empty());  // turning it off applies hot
  CHECK(launcher::RestartNeeded(with(FG::kDlss), with(FG::kOff)).empty());
  CHECK(launcher::RestartNeeded(with(FG::kFsr), with(FG::kFsr)).empty());
  CHECK(launcher::RestartText({F::kFrameGeneration}, launcher::UiLanguage::kItalian) == "Frame generation");
  CHECK(launcher::RestartText({F::kFrameGeneration}, launcher::UiLanguage::kEnglish) == "Frame generation");
}

TEST_CASE("launcher: frame generation texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    const std::string tip = t.frame_generation_tip;
    CHECK(tip.find("FSR 3") != std::string::npos);
    CHECK(tip.find("borderless") != std::string::npos);
    CHECK(tip.find(l == L::kEnglish ? "next start" : "prossimo avvio") != std::string::npos);
    CHECK(std::string(t.frame_generation_unavailable).find("amd_fidelityfx_framegeneration_dx12.dll") !=
          std::string::npos);
  }
}

TEST_CASE("launcher: RestartNeeded for HDR", "[launcher]") {
  using F = launcher::RestartField;
  settings::Settings off, on;
  on.video.hdr = true;
  CHECK(launcher::RestartNeeded(off, on) == std::vector<F>{F::kHdr});
  CHECK(launcher::RestartNeeded(on, off) == std::vector<F>{F::kHdr});  // turning it off also applies from the restart
  CHECK(launcher::RestartNeeded(on, on).empty());
  settings::Settings on_peak = on;
  on_peak.video.hdr_peak_nits = 1000;
  CHECK(launcher::RestartNeeded(on, on_peak) == std::vector<F>{F::kHdr});
  settings::Settings off_peak = off;
  off_peak.video.hdr_peak_nits = 1000;
  CHECK(launcher::RestartNeeded(off, off_peak).empty());  // HDR off: the levels do not matter
  settings::Settings white = on;
  white.video.hdr_paper_white_nits = 250;
  CHECK(launcher::RestartNeeded(on, white) == std::vector<F>{F::kHdr});
  settings::Settings on1000 = on, off1000 = off;
  on1000.video.hdr_peak_nits = 1000;
  off1000.video.hdr_peak_nits = 1000;
  CHECK(launcher::RestartNeeded(on1000, off1000) == std::vector<F>{F::kHdr});
  CHECK(launcher::RestartText({F::kHdr}, launcher::UiLanguage::kItalian) == "HDR");
  CHECK(launcher::RestartText({F::kHdr}, launcher::UiLanguage::kEnglish) == "HDR");
}

TEST_CASE("launcher: AutoHdrLevels", "[launcher]") {
  using L = hdr::Levels;
  CHECK(launcher::AutoHdrLevels(0, {true, 418, 240}) == L{418, 240});
  CHECK(launcher::AutoHdrLevels(400, {true, 0, 480}) == L{1000, 400});
  CHECK(launcher::AutoHdrLevels(0, {true, 0, 480}) == L{1000, 480});
  CHECK(launcher::AutoHdrLevels(0, {false, 418, 80}) == L{1000, 203});
}

TEST_CASE("launcher: HDR texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    const std::string tip = t.hdr_tip;
    CHECK(tip.find("HDR10") != std::string::npos);
    CHECK(tip.find(l == L::kEnglish ? "paper white" : "bianco carta") != std::string::npos);
    CHECK(tip.find(l == L::kEnglish ? "next start" : "prossimo avvio") != std::string::npos);
    CHECK(std::string(t.hdr_unavailable).find(l == L::kEnglish ? "Windows HDR" : "HDR di Windows") != std::string::npos);
    CHECK(std::string(t.hdr_auto).find("%u") != std::string::npos);
  }
}

TEST_CASE("launcher: InternalResolutionLabel", "[launcher]") {
  using A = settings::Aspect;
  CHECK(launcher::InternalResolutionLabel(A::k16x9, 1) == "1x (1280x720)");
  CHECK(launcher::InternalResolutionLabel(A::k16x9, 3) == "3x (3840x2160)");
  CHECK(launcher::InternalResolutionLabel(A::k21x9, 1) == "1x (1280x550)");
  CHECK(launcher::InternalResolutionLabel(A::k21x9, 2) == "2x (2560x1100)");
  CHECK(launcher::InternalResolutionLabel(A::k16x10, 1) == "1x (1280x800)");
  CHECK(launcher::InternalResolutionLabel(A::k16x10, 3) == "3x (3840x2400)");
  CHECK(launcher::InternalResolutionLabel(A::k16x9, 0) == "1x (1280x720)");
  CHECK(launcher::InternalResolutionLabel(A::k16x9, 9) == "3x (3840x2160)");
}

TEST_CASE("launcher: RestartNeeded for the aspect ratio", "[launcher]") {
  using F = launcher::RestartField;
  settings::Settings a, b;
  b.video.aspect = settings::Aspect::k21x9;
  CHECK(launcher::RestartNeeded(a, b) == std::vector<F>{F::kAspect});
  CHECK(launcher::RestartNeeded(b, b).empty());
  CHECK(launcher::RestartText({F::kAspect}, launcher::UiLanguage::kItalian) == "Formato");
  CHECK(launcher::RestartText({F::kAspect}, launcher::UiLanguage::kEnglish) == "Aspect ratio");
}

TEST_CASE("launcher: aspect ratio texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const std::string tip = launcher::Text(l).aspect_tip;
    for (const char* w : {"MSAA", "21:9", "16:10", "1280x800", "boma", "TudorHorse"})
      CHECK(tip.find(w) != std::string::npos);
    CHECK(tip.find("illusion") == std::string::npos);  // the Black Shading Fix is gone
    CHECK(tip.find(l == L::kEnglish ? "next start" : "prossimo avvio") != std::string::npos);
    // fit keeps the scene's shape (21:9, 16:10): the label does not promise bars at 16:9.
    CHECK(std::string(launcher::Text(l).letterbox).find("16:9") == std::string::npos);
  }
}

TEST_CASE("launcher: engine configuration texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    for (const char* tip : {t.shadow_quality_tip, t.dynamic_shadows_tip}) {
      CHECK(std::string(tip).find("Sowa_95") != std::string::npos);
      CHECK(std::string(tip).find("boma") != std::string::npos);
    }
    for (const char* n : {"256", "800", "2048"}) CHECK(std::string(t.shadow_quality_tip).find(n) != std::string::npos);
  }
}

TEST_CASE("launcher: frame rate and VRR texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    CHECK(std::string(t.frame_rate_tip).find("illusion") != std::string::npos);
    CHECK(std::string(t.frame_rate_tip).find("boma") != std::string::npos);
    CHECK(std::string(t.vrr_tip).find("NVAPI") != std::string::npos);
  }
  CHECK(std::string(launcher::Text(L::kItalian).frame_rate_original) == "30 (originale)");
  CHECK(std::string(launcher::Text(L::kItalian).vrr_on) == "S\xC3\xAC");
  CHECK(std::string(launcher::Text(L::kEnglish).vrr_auto) == "Auto");
}

TEST_CASE("launcher: game patch texts", "[launcher]") {
  using L = launcher::UiLanguage;
  CHECK(std::string(launcher::Text(L::kItalian).patch_show_fps) == "Contatore di fps");
  CHECK(std::string(launcher::Text(L::kEnglish).patch_show_fps) == "FPS counter");
  CHECK(std::string(launcher::Text(L::kItalian).patch_no_bloom) == "Senza bloom");
  CHECK(std::string(launcher::Text(L::kEnglish).patch_no_lens_flares) == "No lens flares");
  CHECK(std::string(launcher::Text(L::kEnglish).patch_no_motion_blur_tip).find("illusion") != std::string::npos);
  CHECK(std::string(launcher::Text(L::kItalian).patch_no_bloom_tip).find("boma") != std::string::npos);
}

TEST_CASE("launcher: anisotropic filtering texts", "[launcher]") {
  using L = launcher::UiLanguage;
  CHECK(std::string(launcher::Text(L::kItalian).anisotropic) == "Filtro anisotropico");
  CHECK(std::string(launcher::Text(L::kItalian).anisotropic_default) == "Del gioco");
  CHECK(std::string(launcher::Text(L::kEnglish).anisotropic) == "Anisotropic filtering");
  CHECK(std::string(launcher::Text(L::kEnglish).anisotropic_default) == "Game default");
}

TEST_CASE("launcher: UiLanguageFor", "[launcher]") {
  using L = launcher::UiLanguage;
  CHECK(launcher::UiLanguageFor("it", false) == L::kItalian);
  CHECK(launcher::UiLanguageFor("en", true) == L::kEnglish);
  CHECK(launcher::UiLanguageFor("de", true) == L::kEnglish);
  CHECK(launcher::UiLanguageFor("", true) == L::kItalian);
  CHECK(launcher::UiLanguageFor("", false) == L::kEnglish);
}

TEST_CASE("launcher: Text has all the strings in both languages", "[launcher]") {
  const auto& it = launcher::Text(launcher::UiLanguage::kItalian);
  const auto& en = launcher::Text(launcher::UiLanguage::kEnglish);
  // UiText is made only of const char*: all the fields are checked.
  constexpr size_t n = sizeof(launcher::UiText) / sizeof(const char*);
  // Reading it as an array is valid only if the struct is a block of pointers with no padding or other types.
  static_assert(std::is_standard_layout_v<launcher::UiText> && std::is_trivially_copyable_v<launcher::UiText>);
  static_assert(sizeof(launcher::UiText) % sizeof(const char*) == 0 && alignof(launcher::UiText) == alignof(const char*));
  const auto* a = reinterpret_cast<const char* const*>(&it);
  const auto* b = reinterpret_cast<const char* const*>(&en);
  for (size_t i = 0; i < n; ++i) {
    INFO(i);
    REQUIRE(a[i] != nullptr);
    REQUIRE(b[i] != nullptr);
    CHECK(a[i][0] != 0);
    CHECK(b[i][0] != 0);
    // C3 83 is "A tilde" in UTF-8: a sign of text encoded twice (mojibake).
    CHECK(std::string(a[i]).find("\xC3\x83") == std::string::npos);
    CHECK(std::string(b[i]).find("\xC3\x83") == std::string::npos);
    CHECK(std::string(a[i]).find('{') == std::string::npos);  // {game}, {edition}, {exe} all filled
    CHECK(std::string(b[i]).find('{') == std::string::npos);
  }
  CHECK(std::string(it.play) == "Gioca");
  CHECK(std::string(en.play) == "Play");
  CHECK(std::string(it.apply) == "Applica");
  CHECK(std::string(en.apply) == "Apply");
}

TEST_CASE("launcher: WindowSetupFor", "[launcher]") {
  settings::Video v;
  v.width = 1600;
  v.height = 900;
  v.resizable = true;
  auto ws = launcher::WindowSetupFor(v);
  CHECK(ws.window.width == 1600);
  CHECK(ws.window.height == 900);
  CHECK(ws.window.mode == plat::WindowMode::kWindowed);
  CHECK(ws.window.resizable);
  CHECK(ws.mode_width == 0);
  CHECK_FALSE(ws.exclusive);
  v.width = v.height = 0;
  ws = launcher::WindowSetupFor(v);
  CHECK(ws.window.width == 1280);
  CHECK(ws.window.height == 720);
  v.mode = settings::WindowMode::kFullscreen;
  v.width = 2560;
  v.height = 1440;
  ws = launcher::WindowSetupFor(v);
  CHECK(ws.window.mode == plat::WindowMode::kFullscreen);
  CHECK(ws.mode_width == 2560);
  CHECK(ws.mode_height == 1440);
  CHECK(ws.exclusive);
  CHECK(ws.window.width == 1280);
  v.mode = settings::WindowMode::kBorderless;
  ws = launcher::WindowSetupFor(v);
  CHECK_FALSE(ws.exclusive);
  CHECK(ws.mode_width == 0);
}

TEST_CASE("launcher: DrawSettings centered panel with Apply/Close labels", "[launcher]") {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1920, 1080);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  launcher::LauncherState st;
  st.overlay = true;
  st.panel_w = 640;
  st.panel_h = 600;
  launcher::Action got = launcher::Action::kNone;
  for (int f = 0; f < 8 && got == launcher::Action::kNone; ++f) {
    if (f == 3) io.AddKeyEvent(ImGuiKey_Enter, true);
    if (f == 4) io.AddKeyEvent(ImGuiKey_Enter, false);
    ImGui::NewFrame();
    got = launcher::DrawSettings(st);
    if (f == 0) {
      const ImGuiWindow* w = ImGui::FindWindowByName("Settings");
      REQUIRE(w != nullptr);
      CHECK(w->Pos.x == 640);
      CHECK(w->Pos.y == 240);
      CHECK(w->Size.x == 640);
      CHECK(w->Size.y == 600);
    }
    ImGui::Render();
  }
  CHECK(got == launcher::Action::kPlay);
  ImGui::DestroyContext();
}

TEST_CASE("launcher: panel without scrolling", "[launcher]") {
  for (const char* lang : {"it", "en"}) {
    CAPTURE(lang);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    launcher::LoadFonts();
    launcher::ApplyScale(1);
    launcher::LauncherState st;
    st.s.language = lang;  // "it"/"en"
    st.overlay = true;
    st.panel_w = launcher::kWindowWidth;
    st.panel_h = launcher::kWindowHeight;
    st.hdr_display.hdr = false;
    st.s.video.hdr = true;
    st.s.video.frame_generation = settings::FrameGeneration::kDlss;
    st.fg_available = false;
    st.dlss_fg_available = false;
    st.s.video.upscaler = settings::Upscaler::kDlss;
    st.dlss_available = false;
    st.s.video.dlss_mode = settings::DlssMode::kDlaa;
    st.notice = launcher::Notice::kRestart;
    st.restart = {launcher::RestartField::kUpscaler,
                  launcher::RestartField::kInternalResolution, launcher::RestartField::kEnhancedShaders,
                  launcher::RestartField::kLanguage,
                  launcher::RestartField::kFrameRate, launcher::RestartField::kShadowQuality,
                  launcher::RestartField::kDynamicShadows, launcher::RestartField::kFrameGeneration, launcher::RestartField::kAspect,
                  launcher::RestartField::kHdr, launcher::RestartField::kDlssPreset,
                  launcher::RestartField::kDlssMode, launcher::RestartField::kConsole};
    for (int f = 0; f < 3; ++f) {
      ImGui::NewFrame();
      launcher::DrawSettings(st);
      ImGui::Render();
    }
    const ImGuiWindow* w = ImGui::FindWindowByName("Settings");
    REQUIRE(w != nullptr);
    CHECK(w->ScrollMax.y == 0);
    ImGui::DestroyContext();
  }
}

TEST_CASE("launcher: PanelScale and PanelHeight", "[launcher]") {
  // 100%: exactly FitScale and the usual height, even with the panel taller than the area (FitScale stuck at 0.5).
  const float fit = launcher::FitScale(1.25f, 728, 31);  // 1366x768 at 125%
  CHECK(launcher::PanelScale(1.25f, 728, 31, 100) == fit);
  CHECK(launcher::PanelHeight(fit, 100, 728 - 31) == launcher::kWindowHeight * fit);
  CHECK(launcher::PanelScale(1.0f, 100, 31, 100) == launcher::FitScale(1.0f, 100, 31));
  CHECK(launcher::PanelHeight(0.5f, 100, 69) == launcher::kWindowHeight * 0.5f);
  // 150%: scale x1.5, height limited to the area (the content scrolls).
  const float big = launcher::PanelScale(1.25f, 728, 31, 150);
  CHECK(big == fit * 1.5f);
  CHECK(launcher::PanelHeight(big, 150, 728 - 31) == 728 - 31);
  // 75%: smaller, no limit.
  CHECK(launcher::PanelScale(1.0f, 1200, 31, 75) == 0.75f);
  CHECK(launcher::PanelHeight(0.75f, 75, 1200 - 31) == launcher::kWindowHeight * 0.75f);
  // Above 100% with a large area: no limit.
  CHECK(launcher::PanelHeight(1.1f, 110, 2000) == launcher::kWindowHeight * 1.1f);
}

TEST_CASE("launcher: panel with the interface scale", "[launcher]") {
  for (const uint32_t ui : {75u, 90u, 125u, 150u}) {
    CAPTURE(ui);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    launcher::LoadFonts();
    const float scale = launcher::PanelScale(1.0f, 2000, 0, ui);  // large area: FitScale 1
    launcher::ApplyScale(scale);
    launcher::LauncherState st;
    st.s.language = "it";
    st.s.video.ui_scale = ui;
    st.overlay = true;
    st.panel_w = launcher::kWindowWidth * scale;
    st.panel_h = launcher::PanelHeight(scale, ui, 800);  // above 100% it is 800 tall: the content does not fit
    for (int f = 0; f < 12; ++f) {
      ImGui::NewFrame();
      launcher::DrawSettings(st);
      ImGui::Render();
    }
    const ImGuiWindow* w = ImGui::FindWindowByName("Settings");
    REQUIRE(w != nullptr);
    if (ui < 100) {
      CHECK(w->ScrollMax.y == 0);
      CHECK_FALSE(w->ScrollbarY);
    } else {
      CHECK(w->Size.y == 800);
      CHECK(w->ScrollMax.y > 0);
      CHECK(w->ScrollbarY);
      CHECK(w->Scroll.y == 0);  // opens at the top, even though the initial focus is on Play/Apply at the bottom
    }
    ImGui::DestroyContext();
  }
}

TEST_CASE("launcher: interface scale in the panel", "[launcher]") {
  for (const char* lang : {"it", "en"}) {
    CAPTURE(lang);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 2 * launcher::kWindowHeight);
    io.DeltaTime = 1.0f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    launcher::LoadFonts();
    launcher::ApplyScale(1);
    launcher::LauncherState st;
    st.s.language = lang;
    st.overlay = true;
    st.panel_w = launcher::kWindowWidth;
    st.panel_h = launcher::kWindowHeight;
    ImGui::NewFrame();
    launcher::DrawSettings(st);
    const float bh = ImGui::GetFrameHeight();
    ImGui::Render();
    ImGuiWindow* w = ImGui::FindWindowByName("Settings");
    REQUIRE(w != nullptr);
    const ImGuiID id = w->GetID("##ui_scale");
    // Center of the button row, at the bottom of the panel (at 100% it does not scroll).
    const float y = w->Pos.y + w->Size.y - ImGui::GetStyle().WindowPadding.y - bh / 2;
    bool hovered = false;
    for (float x = w->Pos.x + 1; x < w->Pos.x + w->Size.x && !hovered; x += 4) {
      io.AddMousePosEvent(x, y);
      ImGui::NewFrame();
      launcher::DrawSettings(st);
      hovered = ImGui::GetCurrentContext()->HoveredId == id;
      ImGui::Render();
    }
    CHECK(hovered);
    ImGui::DestroyContext();
  }
}

TEST_CASE("launcher: interface scale texts", "[launcher]") {
  using L = launcher::UiLanguage;
  CHECK(std::string(launcher::Text(L::kItalian).ui_scale) == "Scala interfaccia");
  CHECK(std::string(launcher::Text(L::kEnglish).ui_scale) == "UI scale");
  CHECK(std::string(launcher::Text(L::kItalian).ui_scale_tip).find("subito") != std::string::npos);
  CHECK(std::string(launcher::Text(L::kEnglish).ui_scale_tip).find("immediately") != std::string::npos);
  for (const L l : {L::kEnglish, L::kItalian})
    CHECK(std::string(launcher::Text(l).ui_scale_tip).find("100%") != std::string::npos);
}

TEST_CASE("launcher: the patch tooltips do not say 'from the next launch'", "[launcher]") {
  // They apply hot from the overlay.
  for (const auto l : {launcher::UiLanguage::kItalian, launcher::UiLanguage::kEnglish}) {
    const launcher::UiText& t = launcher::Text(l);
    for (const char* s : {t.patch_show_fps_tip, t.patch_no_motion_blur_tip, t.patch_no_lens_flares_tip,
                          t.patch_no_bloom_tip}) {
      CHECK(std::string(s).find("prossimo avvio") == std::string::npos);
      CHECK(std::string(s).find("next launch") == std::string::npos);
    }
  }
}

TEST_CASE("fg_combo_locked_with_optiscaler", "[launcher]") {
  // How many panel rows open a popup when clicked: with OptiScaler installed the frame generation
  // combo is disabled, so there is one fewer, and the value does not change.
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640, 2000);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  const auto open_rows = [&](optiscaler::State os, settings::FrameGeneration* fg_after) {
    int rows = 0;
    for (float y = 10; y < 1990; y += 6) {
      launcher::LauncherState st;
      st.optiscaler = os;
      st.s.video.frame_generation = settings::FrameGeneration::kDlss;
      st.dlss_fg_available = true;
      st.fg_available = true;
      const auto frame = [&] {
        ImGui::NewFrame();
        launcher::DrawSettings(st);
        ImGui::Render();
      };
      io.AddMousePosEvent(500, y);
      frame();
      frame();
      io.AddMouseButtonEvent(0, true);
      frame();
      io.AddMouseButtonEvent(0, false);
      frame();
      if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ++rows;
      if (fg_after && os == optiscaler::State::kInstalled) CHECK(st.s.video.frame_generation == *fg_after);
      if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
    }
    return rows;
  };
  auto keep = settings::FrameGeneration::kDlss;
  const int free_rows = open_rows(optiscaler::State::kMissing, nullptr);
  const int locked_rows = open_rows(optiscaler::State::kInstalled, &keep);
  REQUIRE(free_rows > 1);
  CHECK(locked_rows < free_rows);
  ImGui::DestroyContext();
}

namespace {
// Text drawn by the panel in a frame (ImGui log: button labels included, tooltips not).
std::string PanelText(launcher::LauncherState st) {
  ImGui::NewFrame();
  ImGui::LogToBuffer();
  launcher::DrawSettings(st);
  std::string out = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  ImGui::Render();
  return out;
}

// Actions obtained by clicking along the controls column, a new state per click.
std::set<launcher::Action> ClickActions(const launcher::LauncherState& base) {
  ImGuiIO& io = ImGui::GetIO();
  std::set<launcher::Action> got;
  for (float y = 10; y < 1990; y += 6) {
    launcher::LauncherState st = base;
    const auto frame = [&] {
      ImGui::NewFrame();
      got.insert(launcher::DrawSettings(st));
      ImGui::Render();
    };
    io.AddMousePosEvent(500, y);
    frame();
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
  }
  return got;
}
}  // namespace

TEST_CASE("launcher: DLSS download button", "[launcher]") {
  using A = launcher::Action;
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640, 2000);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  launcher::LauncherState st;
  st.overlay = true;
  st.s.language = "en";
  st.optiscaler = optiscaler::State::kForeign;  // text only: no OptiScaler button
  const auto has = [](const std::string& text, const char* what) { return text.find(what) != std::string::npos; };
  CHECK_FALSE(has(PanelText(st), "Download NVIDIA DLSS"));
  CHECK_FALSE(ClickActions(st).count(A::kDownloadDlss));
  st.dlss_download_available = true;
  CHECK(has(PanelText(st), "Download NVIDIA DLSS"));
  CHECK(has(PanelText(st), "covered by NVIDIA's license"));
  CHECK(ClickActions(st).count(A::kDownloadDlss));
  st.s.language = "it";
  CHECK(has(PanelText(st), "Scarica NVIDIA DLSS"));
  st.dlss_downloading = true;  // button off while running
  CHECK_FALSE(ClickActions(st).count(A::kDownloadDlss));
  ImGui::DestroyContext();
}

TEST_CASE("optiscaler_button_states", "[launcher]") {
  using A = launcher::Action;
  using S = optiscaler::State;
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640, 2000);
  io.DeltaTime = 1.0f / 60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  const auto has = [](const std::string& text, const char* what) { return text.find(what) != std::string::npos; };
  const auto state = [](S os) {
    launcher::LauncherState st;
    st.s.language = "it";
    st.optiscaler = os;
    st.dlssnr_runtime = true;
    st.s.video.upscaler = settings::Upscaler::kDlss;
    st.dlss_available = true;
    return st;
  };
  const auto pressable = [](const std::set<A>& got) { return got.count(A::kInstallOptiScaler) + got.count(A::kOpenOptiScaler); };

  auto st = state(S::kMissing);
  CHECK(has(PanelText(st), "Rendering neurale"));
  CHECK(has(PanelText(st), "Installa OptiScaler"));
  CHECK(ClickActions(st).count(A::kInstallOptiScaler));
  st.s.language = "en";
  CHECK(has(PanelText(st), "Install OptiScaler"));

  st = state(S::kOther);
  CHECK(has(PanelText(st), "Sostituisci con la v0.1.34"));
  CHECK(ClickActions(st).count(A::kInstallOptiScaler));

  st = state(S::kInstalled);
  st.overlay = true;
  st.optiscaler_loaded = true;
  CHECK(has(PanelText(st), "Apri OptiScaler"));
  {
    const auto got = ClickActions(st);
    CHECK(got.count(A::kOpenOptiScaler));
    CHECK_FALSE(got.count(A::kInstallOptiScaler));
  }

  st = state(S::kInstalled);  // launcher: OptiScaler is not loaded
  CHECK(has(PanelText(st), "OptiScaler installato"));
  CHECK_FALSE(has(PanelText(st), "Apri OptiScaler"));
  CHECK(pressable(ClickActions(st)) == 0);

  st = state(S::kInstalled);
  st.overlay = true;
  st.optiscaler_restart = true;
  CHECK(has(PanelText(st), "Installato: riavvia il gioco"));
  CHECK(pressable(ClickActions(st)) == 0);

  st = state(S::kMissing);
  st.optiscaler_installing = true;
  CHECK(has(PanelText(st), "Installazione..."));
  CHECK(pressable(ClickActions(st)) == 0);

  st = state(S::kForeign);
  CHECK(has(PanelText(st), "dxgi.dll di un altro programma: OptiScaler non si installa"));
  CHECK(pressable(ClickActions(st)) == 0);

  st = state(S::kMissing);
  CHECK_FALSE(has(PanelText(st), "nvngx_dlssnr.dll non fornita dal progetto"));
  CHECK_FALSE(has(PanelText(st), "OptiScaler applica il rendering neurale solo con DLSS"));
  st.dlssnr_runtime = false;
  CHECK(has(PanelText(st), "nvngx_dlssnr.dll non fornita dal progetto"));
  CHECK(ClickActions(st).count(A::kInstallOptiScaler));  // the note does not turn the button off
  st.s.video.upscaler = settings::Upscaler::kFsr;
  CHECK(has(PanelText(st), "OptiScaler applica il rendering neurale solo con DLSS"));

  st = state(S::kMissing);
  st.optiscaler_message = "download failed";
  CHECK(has(PanelText(st), "download failed"));
  ImGui::DestroyContext();
}

TEST_CASE("launcher: OptiScaler texts", "[launcher]") {
  using L = launcher::UiLanguage;
  for (const L l : {L::kEnglish, L::kItalian}) {
    const auto& t = launcher::Text(l);
    const std::string tip = t.optiscaler_tip;
    for (const char* w : {"%s", "GPL-3", "GitHub", "gow2.exe", "nvngx_dlssnr.dll"}) CHECK(tip.find(w) != std::string::npos);
    CHECK(std::string(t.optiscaler_replace).find("%s") != std::string::npos);
  }
  CHECK(std::string(launcher::Text(L::kItalian).optiscaler_replace_tip) == " Sovrascrive l'OptiScaler presente.");
}
