#include "launcher/launcher_model.h"

#include <algorithm>
#include <array>

#include "game_patches/game_patches.h"
#include "game_profile.h"

namespace launcher {

std::vector<Resolution> ResolutionChoices(std::vector<Resolution> modes, uint32_t cur_w, uint32_t cur_h) {
  modes.emplace_back(1280, 720);
  if (cur_w && cur_h) modes.emplace_back(cur_w, cur_h);
  std::erase_if(modes, [](const Resolution& r) { return r.first < 640 || r.second < 480; });
  std::sort(modes.begin(), modes.end(), [](const Resolution& a, const Resolution& b) {
    const uint64_t aa = uint64_t(a.first) * a.second, ba = uint64_t(b.first) * b.second;
    return aa != ba ? aa > ba : a.first > b.first;
  });
  modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
  return modes;
}

std::span<const LanguageChoice> Languages() {
  // ja, ko, zh: Segoe UI has no CJK glyphs, so English name and code.
  static constexpr std::array<LanguageChoice, 12> kList{{
      {"en", "English"},
      {"ja", "Japanese (ja)"},
      {"de", "Deutsch"},
      {"fr", "Fran\xC3\xA7" "ais"},
      {"es", "Espa\xC3\xB1ol"},
      {"it", "Italiano"},
      {"ko", "Korean (ko)"},
      {"zh", "Chinese, traditional (zh)"},
      {"zh-cn", "Chinese, simplified (zh-cn)"},
      {"pt", "Portugu\xC3\xAAs"},
      {"pl", "Polski"},
      {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"},
  }};
  return kList;
}

std::string_view LanguageLabel(std::string_view code) {
  if (code.empty()) code = "en";
  for (const auto& l : Languages())
    if (l.code == code) return l.label;
  return code;
}

float FitScale(float dpi_scale, int work_h, int frame_h) {
  return std::max(0.5f, std::min(dpi_scale, float(work_h - frame_h) / kWindowHeight));
}

float PanelScale(float dpi_scale, int work_h, int frame_h, uint32_t ui_scale) {
  return FitScale(dpi_scale, work_h, frame_h) * (ui_scale / 100.0f);  // 100 / 100.0f == 1.0f: exact
}

float PanelHeight(float scale, uint32_t ui_scale, int avail) {
  const float h = kWindowHeight * scale;
  return ui_scale > 100 ? std::min(h, float(avail)) : h;
}

std::string InternalResolutionLabel(settings::Aspect aspect, uint32_t n) {
  using A = settings::Aspect;
  n = std::clamp(n, 1u, 3u);
  const uint32_t h = aspect == A::k21x9 ? 550 : aspect == A::k16x10 ? 800 : 720;
  return std::to_string(n) + "x (" + std::to_string(1280 * n) + "x" + std::to_string(h * n) + ")";
}

std::string DlssModeLabel(settings::DlssMode m, std::optional<uint32_t> n, settings::Aspect aspect, uint32_t image_w,
                          uint32_t image_h, UiLanguage l) {
  using M = settings::DlssMode;
  const UiText& t = Text(l);
  const char* names[] = {t.dlss_mode_auto, "DLAA", t.dlss_mode_quality, t.dlss_mode_balanced,
                         t.dlss_mode_performance, t.dlss_mode_ultra_performance};
  std::string out = names[size_t(m) < std::size(names) ? size_t(m) : size_t(M::kAuto)];
  if (!n) return out;
  const uint32_t h = aspect == settings::Aspect::k21x9 ? 550 : aspect == settings::Aspect::k16x10 ? 800 : 720;
  return out + " - " + std::to_string(1280 * *n) + "x" + std::to_string(h * *n) + " -> " + std::to_string(image_w) +
         "x" + std::to_string(image_h);
}

hdr::Levels AutoHdrLevels(uint32_t peak_setting, const hdr::Display& display) {
  const hdr::Levels screen = hdr::ChooseLevels(0, 0, display);
  const hdr::Levels chosen = hdr::ChooseLevels(peak_setting, 0, display);
  return {screen.peak_nits, chosen.paper_white_nits};
}

std::vector<RestartField> RestartNeeded(const settings::Settings& running, const settings::Settings& wanted) {
  const settings::Video &a = running.video, &b = wanted.video;
  std::vector<RestartField> out;
  if (a.upscaler != b.upscaler) out.push_back(RestartField::kUpscaler);
  if (a.internal_resolution != b.internal_resolution) out.push_back(RestartField::kInternalResolution);
  if (a.enhanced_shaders != b.enhanced_shaders) out.push_back(RestartField::kEnhancedShaders);
  if (running.language != wanted.language) out.push_back(RestartField::kLanguage);
  if (running.frame_rate != wanted.frame_rate) out.push_back(RestartField::kFrameRate);
  const settings::Engine &ea = running.engine, &eb = wanted.engine;
  if (ea.shadow_quality != eb.shadow_quality) out.push_back(RestartField::kShadowQuality);
  if (ea.dynamic_shadows != eb.dynamic_shadows) out.push_back(RestartField::kDynamicShadows);
  if (b.frame_generation != settings::FrameGeneration::kOff && a.frame_generation != b.frame_generation) out.push_back(RestartField::kFrameGeneration);
  if (a.aspect != b.aspect) out.push_back(RestartField::kAspect);
  // Levels: they matter only if HDR is wanted (off, the peak changes nothing).
  if (a.hdr != b.hdr || (b.hdr && (a.hdr_peak_nits != b.hdr_peak_nits || a.hdr_paper_white_nits != b.hdr_paper_white_nits)))
    out.push_back(RestartField::kHdr);
  if (a.dlss_preset != b.dlss_preset) out.push_back(RestartField::kDlssPreset);
  if (a.dlss_mode != b.dlss_mode) out.push_back(RestartField::kDlssMode);
  if (running.console != wanted.console) out.push_back(RestartField::kConsole);
  return out;
}

static_assert(int(plat::WindowMode::kFullscreen) == int(settings::WindowMode::kFullscreen) &&
              int(plat::WindowMode::kBorderless) == int(settings::WindowMode::kBorderless));

WindowSetup WindowSetupFor(const settings::Video& v) {
  WindowSetup ws;
  ws.window.mode = plat::WindowMode(v.mode);
  ws.window.resizable = v.resizable;
  if (v.mode == settings::WindowMode::kWindowed && v.width != 0) {
    ws.window.width = v.width;
    ws.window.height = v.height;
  }
  if (v.mode == settings::WindowMode::kFullscreen) {
    ws.mode_width = v.width;
    ws.mode_height = v.height;
    ws.exclusive = true;
  }
  return ws;
}

std::set<std::string> UnavailableOptions() {
  using namespace game_patches;
  std::set<std::string> out;
  for (const Patch& p : All())
    if (!p.internal && !p.available) out.insert(p.key);
  if (!Available(kAspect21x9) || !Available(kAspect16x10)) out.insert("aspect");
  if (!Available(kFrameRateInterval) || !Available(kFrameRateUnlock)) out.insert("frame_rate");
  if (game_profile::Get().cooked_dir.empty() || !Available(kCoalescedNoHashCheck)) out.insert("engine");
  if (!game_profile::Get().post.hdr) out.insert("hdr");  // no HDR reconstruction for this game's post processing
  return out;
}

}  // namespace launcher
