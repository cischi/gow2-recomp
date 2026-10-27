#include "launcher/launcher_ui.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>

namespace launcher {
namespace {

// "label | control" row of the two-column table.
void Row(const char* label) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-FLT_MIN);
}

// Tooltip of the last control, wrapped at about 35 characters (SetItemTooltip does not wrap and gets cut off).
void ItemTooltip(const char* text) {
  if (!ImGui::BeginItemTooltip()) return;
  ImGui::PushTextWrapPos(35.0f * ImGui::GetFontSize());
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// tips: tooltip per entry (nullptr = none).
template <typename E, size_t N>
void EnumCombo(const char* id, E& value, const std::array<const char*, N>& names,
               const std::array<const char*, N>* tips = nullptr) {
  const size_t cur = size_t(value) < N ? size_t(value) : 0;
  if (ImGui::BeginCombo(id, names[cur])) {
    for (size_t i = 0; i < N; ++i) {
      if (ImGui::Selectable(names[i], i == cur)) value = E(i);
      if (tips && (*tips)[i]) ItemTooltip((*tips)[i]);
    }
    ImGui::EndCombo();
  }
}

// Row with a nits dropdown: 0 = "Auto (N nit)" (N = the value ChooseLevels would give), then the values; one from the
// file that is not in the list is shown as "N nit".
void NitsCombo(const char* label, const char* id, uint32_t& value, std::initializer_list<uint32_t> choices,
               float auto_nits, const UiText& tx) {
  char cur[48], item[48];
  const auto name = [&](char* out, uint32_t n) {
    if (n == 0) std::snprintf(out, 48, tx.hdr_auto, unsigned(auto_nits + 0.5f));
    else std::snprintf(out, 48, "%u nit", unsigned(n));
  };
  Row(label);
  name(cur, value);
  if (ImGui::BeginCombo(id, cur)) {
    name(item, 0);
    if (ImGui::Selectable(item, value == 0)) value = 0;
    for (const uint32_t n : choices) {
      name(item, n);
      if (ImGui::Selectable(item, value == n)) value = n;
    }
    ImGui::EndCombo();
  }
}

// Dropdown for [video] ui_scale (settings::kUiScales), as wide as "150%". Returns true if the value changed.
bool UiScaleCombo(uint32_t& value) {
  char cur[16];
  std::snprintf(cur, sizeof cur, "%u%%", unsigned(value));
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("150%").x + ImGui::GetStyle().FramePadding.x * 2 +
                          ImGui::GetFrameHeight());
  const uint32_t before = value;
  if (ImGui::BeginCombo("##ui_scale", cur)) {
    for (const uint32_t n : settings::kUiScales) {
      char item[16];
      std::snprintf(item, sizeof item, "%u%%", unsigned(n));
      if (ImGui::Selectable(item, value == n)) value = n;
    }
    ImGui::EndCombo();
  }
  return value != before;
}

// Translated title, fixed table id (does not change with the language).
bool BeginSection(const char* title, const char* id) {
  ImGui::SeparatorText(title);
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
  ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.58f);
  return true;
}

void DrawScreen(settings::Video& v, const std::vector<Resolution>& resolutions, const UiText& tx,
                const hdr::Display& hdr_display) {
  if (!BeginSection(tx.display, "##screen")) return;
  Row(tx.mode);
  EnumCombo("##mode", v.mode, std::array{tx.windowed, tx.borderless, tx.fullscreen});

  const bool borderless = v.mode == settings::WindowMode::kBorderless;
  Row(tx.resolution);
  ImGui::BeginDisabled(borderless);
  char cur[64];
  std::snprintf(cur, sizeof cur, "%s", tx.res_default);
  if (borderless) std::snprintf(cur, sizeof cur, "%s", tx.res_monitor);
  else if (v.width && v.height) std::snprintf(cur, sizeof cur, "%ux%u", v.width, v.height);
  if (ImGui::BeginCombo("##resolution", cur)) {
    if (ImGui::Selectable(tx.res_default, v.width == 0)) v.width = v.height = 0;
    for (const auto& [w, h] : resolutions) {
      char item[32];
      std::snprintf(item, sizeof item, "%ux%u", w, h);
      if (ImGui::Selectable(item, v.width == w && v.height == h)) {
        v.width = w;
        v.height = h;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();

  Row(tx.resizable);
  ImGui::BeginDisabled(v.mode != settings::WindowMode::kWindowed);
  ImGui::Checkbox("##resizable", &v.resizable);
  ImGui::EndDisabled();
  Row(tx.scaling);
  EnumCombo("##scaling", v.scaling, std::array{tx.letterbox, tx.stretch});
  Row(tx.aspect);
  EnumCombo("##aspect", v.aspect, std::array{"16:9", "21:9", "16:10"});
  ItemTooltip(tx.aspect_tip);
  Row(tx.vsync);
  ImGui::Checkbox("##vsync", &v.vsync);
  Row(tx.vrr);
  EnumCombo("##vrr", v.vrr, std::array{tx.vrr_auto, tx.vrr_on, tx.vrr_off});
  ItemTooltip(tx.vrr_tip);
  Row(tx.hdr);
  ImGui::BeginDisabled(!hdr_display.hdr && !v.hdr);  // can be turned off even without an HDR output
  ImGui::Checkbox("##hdr", &v.hdr);
  ImGui::EndDisabled();
  ItemTooltip(hdr_display.hdr ? tx.hdr_tip : tx.hdr_unavailable);
  if (v.hdr && !hdr_display.hdr) ImGui::TextWrapped("%s", tx.hdr_unavailable);
  const hdr::Levels auto_levels = AutoHdrLevels(v.hdr_peak_nits, hdr_display);
  ImGui::BeginDisabled(!v.hdr);
  NitsCombo(tx.hdr_peak, "##hdr_peak", v.hdr_peak_nits, {400, 600, 800, 1000, 1500, 2000, 4000},
            auto_levels.peak_nits, tx);
  ItemTooltip(tx.hdr_peak_tip);
  NitsCombo(tx.hdr_paper_white, "##hdr_paper_white", v.hdr_paper_white_nits, {80, 100, 120, 160, 203, 250, 300, 400},
            auto_levels.paper_white_nits, tx);
  ItemTooltip(tx.hdr_paper_white_tip);
  ImGui::EndDisabled();
  ImGui::EndTable();
}

// "Neural rendering" row: OptiScaler button or state, then the notes. Returns the button pressed.
Action OptiScalerRow(const LauncherState& st, const UiText& tx) {
  using S = optiscaler::State;
  Row(tx.neural_rendering);
  Action action = Action::kNone;
  char label[96], tip[512];
  const std::string version(optiscaler::kVersion);
  std::snprintf(tip, sizeof tip, tx.optiscaler_tip, version.c_str());
  const auto button = [&](const char* text, Action a) {
    if (ImGui::Button(text, ImVec2(-FLT_MIN, 0))) action = a;
  };
  if (st.optiscaler_installing) {
    ImGui::BeginDisabled();
    button(tx.optiscaler_installing, Action::kNone);
    ImGui::EndDisabled();
  } else if (st.optiscaler_restart) {
    ImGui::TextWrapped("%s", tx.optiscaler_restart);
  } else if (st.optiscaler == S::kForeign) {
    ImGui::TextWrapped("%s", tx.optiscaler_foreign);
  } else if (st.optiscaler == S::kInstalled) {
    // In the launcher OptiScaler is not loaded: no menu to open.
    if (st.overlay && st.optiscaler_loaded) button(tx.optiscaler_open, Action::kOpenOptiScaler);
    else ImGui::TextWrapped("%s", tx.optiscaler_installed);
  } else if (st.optiscaler == S::kOther) {
    std::snprintf(label, sizeof label, tx.optiscaler_replace, version.c_str());
    button(label, Action::kInstallOptiScaler);
    ItemTooltip((std::string(tip) + tx.optiscaler_replace_tip).c_str());
  } else {
    button(tx.optiscaler_install, Action::kInstallOptiScaler);
    ItemTooltip(tip);
  }
  if (!st.optiscaler_message.empty()) ImGui::TextWrapped("%s", st.optiscaler_message.c_str());
  if (!st.dlssnr_runtime) ImGui::TextWrapped("%s", tx.optiscaler_no_dlssnr);
  if (st.s.video.upscaler != settings::Upscaler::kDlss) ImGui::TextWrapped("%s", tx.optiscaler_needs_dlss);
  return action;
}

Action DrawImage(settings::Video& v, settings::Engine& e, const UiText& tx, UiLanguage lang, bool dlss_available,
                 bool fg_available, bool dlss_fg_available,
                 uint32_t desktop_w, uint32_t desktop_h, const LauncherState& st) {
  if (!BeginSection(tx.image, "##image")) return Action::kNone;
  Row(tx.upscaler);
  using U = settings::Upscaler;
  const bool fsr = v.upscaler == U::kFsr, dlss = v.upscaler == U::kDlss;
  if (ImGui::BeginCombo("##upscaler", fsr ? "AMD FSR" : dlss ? "NVIDIA DLSS" : tx.none)) {
    if (ImGui::Selectable(tx.none, !fsr && !dlss)) v.upscaler = U::kOff;
    if (ImGui::Selectable("AMD FSR", fsr)) v.upscaler = U::kFsr;
    ImGui::BeginDisabled(!dlss_available);
    if (ImGui::Selectable("NVIDIA DLSS", dlss)) v.upscaler = U::kDlss;
    ImGui::EndDisabled();
    if (!dlss_available) ItemTooltip(tx.dlss_unavailable);
    ImGui::EndCombo();
  }
  // dlss in the file but bridge missing: the game will start without an upscaler, which must be said without opening the dropdown.
  if (dlss && !dlss_available) ImGui::TextWrapped("%s", tx.dlss_unavailable);

  Row(tx.dlss_preset);
  ImGui::BeginDisabled(!dlss);
  const std::array<const char*, 7> preset_tips{nullptr,
                                               tx.dlss_preset_tip_ef,
                                               tx.dlss_preset_tip_ef,
                                               tx.dlss_preset_tip_j,
                                               tx.dlss_preset_tip_k,
                                               tx.dlss_preset_tip_l,
                                               tx.dlss_preset_tip_m};
  EnumCombo("##dlss_preset", v.dlss_preset, std::array{tx.dlss_preset_default, "E", "F", "J", "K", "L", "M"},
            &preset_tips);
  ImGui::EndDisabled();
  ItemTooltip(tx.dlss_preset_tip);

  // DLSS mode for the next launch's output (settings + primary monitor, like main.cpp); the entries that the
  // integer render cannot reach are turned off.
  const auto [out_w, out_h] = settings::OutputSizeFor(v, desktop_w, desktop_h);
  const uint32_t image_w = settings::ImageWidthFor(v, out_w, out_h);
  const uint32_t image_h = settings::ImageHeightFor(v, out_w, out_h);
  const auto mode_label = [&](settings::DlssMode m) {
    return DlssModeLabel(m, settings::InternalResolutionForDlssMode(m, image_w), v.aspect, image_w, image_h, lang);
  };
  Row(tx.dlss_mode);
  ImGui::BeginDisabled(!dlss);
  if (ImGui::BeginCombo("##dlss_mode", mode_label(v.dlss_mode).c_str())) {
    for (int i = 0; i <= int(settings::DlssMode::kUltraPerformance); ++i) {
      const auto m = settings::DlssMode(i);
      const bool reachable = m == settings::DlssMode::kAuto || settings::InternalResolutionForDlssMode(m, image_w);
      ImGui::BeginDisabled(!reachable);
      if (ImGui::Selectable(mode_label(m).c_str(), m == v.dlss_mode)) v.dlss_mode = m;
      ImGui::EndDisabled();
      if (!reachable) {
        char tip[160];
        std::snprintf(tip, sizeof tip, tx.dlss_mode_unreachable, unsigned(out_w), unsigned(out_h));
        ItemTooltip(tip);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ItemTooltip(tx.dlss_mode_tip);

  Action action = OptiScalerRow(st, tx);
  if (st.dlss_download_available || st.dlss_downloading) {
    Row(tx.dlss_download);
    ImGui::BeginDisabled(st.dlss_downloading);
    if (ImGui::Button((std::string(st.dlss_downloading ? tx.dlss_downloading : tx.dlss_download) + "###dlss_download").c_str(),
                      ImVec2(-FLT_MIN, 0)) &&
        action == Action::kNone)
      action = Action::kDownloadDlss;
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tx.dlss_download_note);
  }
  if (!st.dlss_download_message.empty()) ImGui::TextWrapped("%s", st.dlss_download_message.c_str());

  Row(tx.frame_generation);
  // Unavailable entries are turned off; the file's value stays as it is (the runtime writes the reason for the fallback to the log).
  using FG = settings::FrameGeneration;
  const auto fg_item = [&](const char* label, FG f, bool available, const char* tip, const char* unavailable) {
    ImGui::BeginDisabled(!available);
    if (ImGui::Selectable(label, v.frame_generation == f)) v.frame_generation = f;
    ImGui::EndDisabled();
    ItemTooltip(available ? tip : unavailable);
  };
  const FG fg_cur = v.frame_generation;
  const bool fg_locked = st.optiscaler == optiscaler::State::kInstalled;  // chosen from OptiScaler's menu
  const char* fg_name = fg_locked ? tx.frame_generation_off
                        : fg_cur == FG::kFsr ? "AMD FSR 3"
                        : fg_cur == FG::kDlss ? "NVIDIA DLSS"
                                              : tx.frame_generation_off;
  ImGui::BeginDisabled(fg_locked);
  if (ImGui::BeginCombo("##frame_generation", fg_name)) {
    if (ImGui::Selectable(tx.frame_generation_off, fg_cur == FG::kOff)) v.frame_generation = FG::kOff;
    fg_item("AMD FSR 3", FG::kFsr, fg_available, tx.frame_generation_tip, tx.frame_generation_unavailable);
    fg_item("NVIDIA DLSS", FG::kDlss, dlss_fg_available, tx.frame_generation_dlss_tip,
            tx.frame_generation_dlss_unavailable);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (fg_locked) ItemTooltip(tx.frame_generation_optiscaler);
  if (!fg_locked && fg_cur == FG::kFsr && !fg_available) ImGui::TextWrapped("%s", tx.frame_generation_unavailable);
  if (!fg_locked && fg_cur == FG::kDlss && !dlss_fg_available) ImGui::TextWrapped("%s", tx.frame_generation_dlss_unavailable);

  Row(tx.antialiasing);
  ImGui::BeginDisabled(fsr || dlss);
  EnumCombo("##aa", v.antialiasing, std::array{tx.none, "FXAA", tx.fxaa_extreme});
  ImGui::EndDisabled();
  if (fsr || dlss) ItemTooltip(tx.upscaler_replaces_fxaa);

  Row(fsr ? tx.sharpness_fsr : tx.sharpness);
  ImGui::BeginDisabled(dlss);
  ImGui::Checkbox("##sharpening", &v.sharpening);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::BeginDisabled(!v.sharpening);
  ImGui::SliderFloat("##sharpness", &v.sharpness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (dlss) ItemTooltip(tx.dlss_no_sharpness);

  Row(tx.internal_resolution);
  // With DLSS and a reachable mode the mode decides it: that one is shown, read-only.
  const auto mode_n = dlss ? settings::InternalResolutionForDlssMode(v.dlss_mode, image_w) : std::nullopt;
  ImGui::BeginDisabled(mode_n.has_value());
  if (ImGui::BeginCombo("##internal", InternalResolutionLabel(v.aspect, mode_n.value_or(v.internal_resolution)).c_str())) {
    for (uint32_t n = 1; n <= 3; ++n)
      if (ImGui::Selectable(InternalResolutionLabel(v.aspect, n).c_str(), v.internal_resolution == n))
        v.internal_resolution = n;
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (mode_n) ItemTooltip(tx.internal_resolution_from_mode);
  Row(tx.anisotropic);
  static constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
  char aniso_cur[16];
  std::snprintf(aniso_cur, sizeof aniso_cur, "%ux", v.anisotropic);
  if (ImGui::BeginCombo("##anisotropic", v.anisotropic ? aniso_cur : tx.anisotropic_default)) {
    for (const uint32_t n : kAniso) {
      char item[16];
      std::snprintf(item, sizeof item, "%ux", n);
      if (ImGui::Selectable(n ? item : tx.anisotropic_default, v.anisotropic == n)) v.anisotropic = n;
    }
    ImGui::EndCombo();
  }
  ItemTooltip(tx.anisotropic_tip);
  Row(tx.enhanced_shaders);
  ImGui::Checkbox("##enhanced", &v.enhanced_shaders);
  Row(tx.shadow_quality);
  EnumCombo("##shadow_quality", e.shadow_quality, std::array{tx.shadow_low, tx.shadow_original, tx.shadow_high});
  ItemTooltip(tx.shadow_quality_tip);
  Row(tx.dynamic_shadows);
  ImGui::Checkbox("##dynamic_shadows", &e.dynamic_shadows);
  ItemTooltip(tx.dynamic_shadows_tip);
  ImGui::EndTable();
  return action;
}

// Checkbox bound to a [patches] key: on if the key is in the set.
void PatchRow(std::set<std::string>& patches, const char* key, const char* label, const char* tip) {
  Row(label);
  bool on = patches.count(key) != 0;
  const std::string id = std::string("##patch_") + key;
  if (ImGui::Checkbox(id.c_str(), &on)) {
    if (on) patches.insert(key);
    else patches.erase(key);
  }
  ItemTooltip(tip);
}

void DrawGame(std::string& language, settings::FrameRate& frame_rate, std::set<std::string>& patches, bool& console,
              const UiText& tx) {
  if (!BeginSection(tx.game, "##game")) return;
  Row(tx.language);
  std::string_view code = language;
  if (code.empty()) code = "en";
  if (ImGui::BeginCombo("##language", std::string(LanguageLabel(code)).c_str())) {
    for (const auto& l : Languages())
      if (ImGui::Selectable(std::string(l.label).c_str(), l.code == code)) language = l.code;
    ImGui::EndCombo();
  }
  Row(tx.frame_rate);
  EnumCombo("##frame_rate", frame_rate,
            std::array{tx.frame_rate_original, "60", "120", "144", tx.frame_rate_monitor, tx.frame_rate_unlimited});
  ItemTooltip(tx.frame_rate_tip);
  PatchRow(patches, "show_fps", tx.patch_show_fps, tx.patch_show_fps_tip);
  PatchRow(patches, "disable_motion_blur", tx.patch_no_motion_blur, tx.patch_no_motion_blur_tip);
  PatchRow(patches, "disable_lens_flares", tx.patch_no_lens_flares, tx.patch_no_lens_flares_tip);
  PatchRow(patches, "disable_bloom", tx.patch_no_bloom, tx.patch_no_bloom_tip);
  Row(tx.console);
  ImGui::Checkbox("##console", &console);
  ItemTooltip(tx.console_tip);
  ImGui::EndTable();
}

}  // namespace

Action DrawSettings(LauncherState& st) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  if (st.panel_w > 0 && st.panel_h > 0) {
    const ImVec2 size(st.panel_w, st.panel_h);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) / 2, vp->WorkPos.y + (vp->WorkSize.y - size.y) / 2));
    ImGui::SetNextWindowSize(size);
  } else {
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
  }
  // NoDecoration without NoScrollbar: above 100% ui_scale the content scrolls; at 100% it fits in the panel and the
  // scrollbar does not appear.
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  Action action = Action::kNone;
  if (st.focus_play) ImGui::SetNextWindowFocus();
  if (ImGui::Begin("Settings", nullptr, kFlags)) {
    const UiText& tx = Text(UiLanguageFor(st.s.language, st.windows_italian));
    if (!st.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
      ImGui::TextWrapped("%s%s", st.save_failed ? tx.save_failed : "", st.error.c_str());
      ImGui::PopStyleColor();
    }
    switch (st.notice) {
      case Notice::kInvalidFile: ImGui::TextWrapped("%s", tx.notice_invalid_file); break;
      case Notice::kCommentsLost: ImGui::TextWrapped("%s", tx.notice_comments_lost); break;
      case Notice::kRestart:
        if (!st.restart.empty())
          ImGui::TextWrapped("%s%s", tx.restart_prefix, RestartText(st.restart, UiLanguageFor(st.s.language, st.windows_italian)).c_str());
        break;
      case Notice::kNone: break;
    }
    if (!st.overlay) {
      ImGui::SeparatorText(tx.game_data);
      ImGui::TextWrapped("%s", st.data_ready ? tx.data_ready : tx.data_missing);
      if (!st.data_message.empty()) ImGui::TextWrapped("%s", st.data_message.c_str());
      if (st.extracting) {
        ImGui::ProgressBar(st.extract_progress, ImVec2(-FLT_MIN, 0));
        if (ImGui::Button((std::string(tx.cancel_extract) + "###cancel_extract").c_str())) action = Action::kCancelExtract;
      } else if (ImGui::Button((std::string(tx.choose_iso) + "###choose_iso").c_str())) {
        action = Action::kChooseIso;
      }
    }
    DrawScreen(st.s.video, st.resolutions, tx, st.hdr_display);
    if (const Action a = DrawImage(st.s.video, st.s.engine, tx, UiLanguageFor(st.s.language, st.windows_italian),
                       st.dlss_available, st.fg_available, st.dlss_fg_available, st.desktop_w,
                       st.desktop_h, st);
        a != Action::kNone)
      action = a;
    DrawGame(st.s.language, st.s.frame_rate, st.s.patches, st.s.console, tx);

    // Buttons at the bottom right; Play/Apply has the initial focus (Enter or the pad's A).
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* play = st.overlay ? tx.apply : tx.play;
    const char* quit = st.overlay ? tx.close : tx.quit;
    float text_w = 0;
    for (const char* l : {tx.defaults, play, quit}) text_w = std::max(text_w, ImGui::CalcTextSize(l).x);
    const float bw = text_w + style.FramePadding.x * 4;
    const float bh = ImGui::GetFrameHeight();
    const float y = ImGui::GetWindowHeight() - bh - style.WindowPadding.y;
    if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    // Interface scale to the left of the buttons, on the same row.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tx.ui_scale);
    ImGui::SameLine();
    // After changing the scale we stay on the dropdown (no jump back to the top): other values can be tried.
    UiScaleCombo(st.s.video.ui_scale);
    ItemTooltip(tx.ui_scale_tip);
    ImGui::SameLine();
    const float scrollbar = ImGui::GetScrollMaxY() > 0 ? style.ScrollbarSize : 0;  // buttons outside the scrollbar
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 3 * bw - 2 * style.ItemSpacing.x - style.WindowPadding.x -
                         scrollbar);
    // Fixed IDs (###): when the Language changes, the focused button stays the same.
    if (ImGui::Button((std::string(tx.defaults) + "###defaults").c_str(), ImVec2(bw, 0))) action = Action::kDefaults;
    ImGui::SameLine();
    if (ImGui::Button((std::string(quit) + "###quit").c_str(), ImVec2(bw, 0))) action = Action::kQuit;
    ImGui::SameLine();
    if (st.focus_play && ImGui::IsWindowFocused()) {
      ImGui::SetKeyboardFocusHere();
      ImGui::SetNavCursorVisible(true);  // without a visible cursor Enter and A do not activate
      st.focus_play = false;
      st.scroll_top_frames = 4;
    }
    // Without game data Play is off, so Enter / pad A cannot start the game either.
    ImGui::BeginDisabled(!st.overlay && (!st.data_ready || st.extracting));
    if (ImGui::Button((std::string(play) + "###play").c_str(), ImVec2(bw, 0))) action = Action::kPlay;
    ImGui::EndDisabled();
    // Focus on Play/Apply would scroll the panel to the bottom (navigation brings the element into view, in the
    // following frames): for a few frames it is scrolled back to the top, so it opens from the top.
    if (st.scroll_top_frames > 0) {
      --st.scroll_top_frames;
      ImGui::SetScrollY(0);
    }
  }
  ImGui::End();
  return action;
}

}  // namespace launcher
