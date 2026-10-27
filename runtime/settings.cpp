#include "settings.h"

#include "gpu/dlss_bridge/gow2_dlss_policy.h"

#include <cctype>
#include <charconv>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

namespace settings {
namespace {

constexpr uint32_t kMinW = 320, kMaxW = 7680, kMinH = 240, kMaxH = 4320;

struct Failure {
  std::string text;
};

[[noreturn]] void Fail(const std::string& file, const toml::node& node, const std::string& key,
                       const std::string& why) {
  std::string s = file;
  if (node.source().begin) s += ":" + std::to_string(node.source().begin.line);
  s += ": " + key + ": " + why;
  throw Failure{s};
}

uint32_t ReadInt(const std::string& file, const toml::node& n, const std::string& key, uint32_t lo, uint32_t hi,
                 const std::string& note = "") {
  const auto v = n.is_integer() ? n.value<int64_t>() : std::nullopt;
  if (!v) Fail(file, n, key, "expected an integer");
  if (*v < lo || *v > hi)
    Fail(file, n, key,
         "value " + std::to_string(*v) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi) + note);
  return uint32_t(*v);
}

std::string Dec1(double d) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", d);
  return b;
}

float ReadFloat(const std::string& file, const toml::node& n, const std::string& key, double lo, double hi) {
  const auto v = (n.is_floating_point() || n.is_integer()) ? n.value<double>() : std::nullopt;
  if (!v) Fail(file, n, key, "expected a number");
  if (!(*v >= lo && *v <= hi)) Fail(file, n, key, "value " + std::to_string(*v) + " out of range " + Dec1(lo) + ".." + Dec1(hi));
  return float(*v);
}

bool ReadBool(const std::string& file, const toml::node& n, const std::string& key) {
  const auto v = n.is_boolean() ? n.value<bool>() : std::nullopt;
  if (!v) Fail(file, n, key, "expected true or false");
  return *v;
}

std::string ReadStr(const std::string& file, const toml::node& n, const std::string& key) {
  const auto v = n.is_string() ? n.value<std::string>() : std::nullopt;
  if (!v) Fail(file, n, key, "expected a string");
  return *v;
}

void Apply(const std::string& file, const toml::table& root, Settings& s) {
  for (auto&& [sec, node] : root) {
    const std::string section(sec.str());
    if (section != "video" && section != "gpu" && section != "game" && section != "patches" &&
        section != "engine" && section != "debug")
      Fail(file, node, section, "unknown section");
    const auto* tbl = node.as_table();
    if (!tbl) Fail(file, node, section, "expected a table");
    for (auto&& [k, v] : *tbl) {
      const std::string key(k.str());
      if (section == "video") {
        if (key == "mode") {
          const auto m = ParseWindowMode(ReadStr(file, v, key));
          if (!m) Fail(file, v, key, "allowed values: \"windowed\", \"borderless\", \"fullscreen\"");
          s.video.mode = *m;
        } else if (key == "width") {
          s.video.width = ReadInt(file, v, key, kMinW, kMaxW);
        } else if (key == "height") {
          s.video.height = ReadInt(file, v, key, kMinH, kMaxH);
        } else if (key == "scaling") {
          const std::string t = ReadStr(file, v, key);
          if (t == "fit") s.video.scaling = Scaling::kFit;
          else if (t == "stretch") s.video.scaling = Scaling::kStretch;
          else Fail(file, v, key, "allowed values: \"fit\", \"stretch\"");
        } else if (key == "vsync") {
          s.video.vsync = ReadBool(file, v, key);
        } else if (key == "resizable") {
          s.video.resizable = ReadBool(file, v, key);
        } else if (key == "antialiasing") {
          const auto a = ParseAntialiasing(ReadStr(file, v, key));
          if (!a) Fail(file, v, key, "allowed values: \"off\", \"fxaa\", \"fxaa_extreme\"");
          s.video.antialiasing = *a;
        } else if (key == "upscaler") {
          const auto u = ParseUpscaler(ReadStr(file, v, key));
          if (!u) Fail(file, v, key, "allowed values: \"off\", \"fsr\", \"dlss\"");
          s.video.upscaler = *u;
        } else if (key == "sharpening") {
          s.video.sharpening = ReadBool(file, v, key);
        } else if (key == "sharpness") {
          s.video.sharpness = ReadFloat(file, v, key, 0.0, 1.0);
        } else if (key == "enhanced_shaders") {
          s.video.enhanced_shaders = ReadBool(file, v, key);
        } else if (key == "internal_resolution") {
          s.video.internal_resolution = ReadInt(file, v, key, 1, 3);
        } else if (key == "anisotropic") {
          const uint32_t a = ReadInt(file, v, key, 0, UINT32_MAX);
          if (AnisotropicOverride(a) < 0 && a != 0) Fail(file, v, key, "allowed values: 0, 2, 4, 8, 16");
          s.video.anisotropic = a;
        } else if (key == "vrr") {
          const auto r = ParseVrr(ReadStr(file, v, key));
          if (!r) Fail(file, v, key, "allowed values: \"auto\", \"on\", \"off\"");
          s.video.vrr = *r;
        } else if (key == "frame_generation") {
          // Boolean (old file: true = fsr) or string.
          if (v.is_boolean()) {
            s.video.frame_generation = *v.value<bool>() ? FrameGeneration::kFsr : FrameGeneration::kOff;
          } else {
            const auto r = ParseFrameGeneration(ReadStr(file, v, key));
            if (!r) Fail(file, v, key, "allowed values: \"off\", \"fsr\", \"dlss\"");
            s.video.frame_generation = *r;
          }
        } else if (key == "hdr") {
          s.video.hdr = ReadBool(file, v, key);
        } else if (key == "hdr_peak_nits") {
          const uint32_t n = ReadInt(file, v, key, 0, kHdrPeakNitsMax, ": 0 (auto) or 100-10000");
          if (n != 0 && n < kHdrPeakNitsMin)
            Fail(file, v, key, "invalid value " + std::to_string(n) + ": 0 (auto) or 100-10000");
          s.video.hdr_peak_nits = n;
        } else if (key == "hdr_paper_white_nits") {
          const uint32_t n = ReadInt(file, v, key, 0, kHdrPaperWhiteNitsMax, ": 0 (auto) or 80-1000");
          if (n != 0 && n < kHdrPaperWhiteNitsMin)
            Fail(file, v, key, "invalid value " + std::to_string(n) + ": 0 (auto) or 80-1000");
          s.video.hdr_paper_white_nits = n;
        } else if (key == "aspect") {
          const auto r = ParseAspect(ReadStr(file, v, key));
          if (!r) Fail(file, v, key, "allowed values: \"16:9\", \"21:9\", \"16:10\"");
          s.video.aspect = *r;
        } else if (key == "dlss_preset") {
          const auto r = ParseDlssPreset(ReadStr(file, v, key));
          if (!r) Fail(file, v, key, "allowed values: \"default\", \"e\", \"f\", \"j\", \"k\", \"l\", \"m\"");
          s.video.dlss_preset = *r;
        } else if (key == "dlss_mode") {
          const auto r = ParseDlssMode(ReadStr(file, v, key));
          if (!r)
            Fail(file, v, key,
                 "allowed values: \"auto\", \"dlaa\", \"quality\", \"balanced\", \"performance\", "
                 "\"ultra_performance\"");
          s.video.dlss_mode = *r;
        } else if (key == "dlss_nr" || key == "dlss_nr_intensity" || key == "dlss_nr_style") {
          // Neural rendering moved to OptiScaler: the key in old files is ignored.
        } else if (key == "ui_scale") {
          const uint32_t n = ReadInt(file, v, key, 0, UINT32_MAX);
          if (std::find(kUiScales.begin(), kUiScales.end(), n) == kUiScales.end())
            Fail(file, v, key, "allowed values: 75, 90, 100, 110, 125, 150");
          s.video.ui_scale = n;
        } else {
          Fail(file, v, key, "unknown key in [video]");
        }
      } else if (section == "patches") {
        if (std::find(kPatchKeys.begin(), kPatchKeys.end(), key) == kPatchKeys.end()) {
          std::string allowed;
          for (const auto c : kPatchKeys) allowed += std::string(allowed.empty() ? "" : ", ") + std::string(c);
          Fail(file, v, key, "unknown key in [patches]; valid keys: " + allowed);
        }
        if (ReadBool(file, v, key)) s.patches.insert(key);
        else s.patches.erase(key);
      } else if (section == "engine") {
        if (key == "shadow_quality") {
          const auto q = ParseShadowQuality(ReadStr(file, v, key));
          if (!q) Fail(file, v, key, "allowed values: \"low\", \"original\", \"high\"");
          s.engine.shadow_quality = *q;
        } else if (key == "dynamic_shadows") {
          s.engine.dynamic_shadows = ReadBool(file, v, key);
        } else if (key == "world_textures") {
          // Option removed (the packages lack the extra mips): the key in old files is ignored.
        } else {
          Fail(file, v, key, "unknown key in [engine]");
        }
      } else if (section == "debug") {
        if (key != "console") Fail(file, v, key, "unknown key in [debug]");
        s.console = ReadBool(file, v, key);
      } else if (section == "game") {
        if (key == "frame_rate") {
          // Numbers as integers, words as strings: "60" (a string) is not allowed.
          std::optional<FrameRate> r;
          if (v.is_integer()) r = ParseFrameRate(std::to_string(*v.value<int64_t>()));
          else if (v.is_string() && !std::isdigit(static_cast<unsigned char>((*v.value<std::string>() + " ")[0])))
            r = ParseFrameRate(*v.value<std::string>());
          if (!r) Fail(file, v, key, "allowed values: 30, 60, 120, 144, \"monitor\", \"unlimited\"");
          s.frame_rate = *r;
          continue;
        }
        if (key != "language") Fail(file, v, key, "unknown key in [game]");
        const std::string code = ReadStr(file, v, key);
        if (std::find(kLanguageCodes.begin(), kLanguageCodes.end(), code) == kLanguageCodes.end()) {
          std::string allowed;
          for (const auto c : kLanguageCodes) allowed += std::string(allowed.empty() ? "" : ", ") + "\"" + std::string(c) + "\"";
          Fail(file, v, key, "allowed values: " + allowed);
        }
        s.language = code;
      } else if (key == "cvars") {
        const auto* cv = v.as_table();
        if (!cv) Fail(file, v, key, "expected a table name = \"value\"");
        for (auto&& [cn, cvn] : *cv) {
          const std::string name(cn.str());
          s.gpu_cvars.emplace_back(name, ReadStr(file, cvn, name));
        }
      } else {
        Fail(file, v, key, "unknown key in [gpu]");
      }
    }
  }
}

}  // namespace

std::optional<std::string> LoadFile(const std::filesystem::path& path, Settings& out) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::nullopt;
  const std::u8string u8 = path.filename().u8string();  // string() can throw on names outside the codepage
  const std::string file(u8.begin(), u8.end());
  std::ifstream in(path, std::ios::binary);
  if (!in || std::filesystem::is_directory(path, ec)) return file + ": cannot read the file";
  try {
    const toml::table root = toml::parse(in, file);  // toml++ skips the UTF-8 BOM and accepts CRLF
    Settings s = out;
    Apply(file, root, s);
    out = std::move(s);
  } catch (const Failure& f) {
    return f.text;
  } catch (const toml::parse_error& e) {
    return file + ":" + std::to_string(e.source().begin.line) + ": " + std::string(e.description());
  } catch (const std::exception& e) {
    return file + ": " + e.what();
  }
  return std::nullopt;
}

std::string Serialize(const Settings& s) {
  const auto q = [](std::string_view v) {
    std::ostringstream o;
    o << toml::value<std::string>(std::string(v));  // toml++ quotes and escapes
    return o.str();
  };
  const Video& v = s.video;
  static constexpr const char* kMode[] = {"windowed", "borderless", "fullscreen"};
  static constexpr const char* kAa[] = {"off", "fxaa", "fxaa_extreme"};
  static constexpr const char* kUp[] = {"off", "fsr", "dlss"};
  // Shortest form that reads back identically (the slider gives values like 0.537).
  char sharp[32];
  *std::to_chars(sharp, sharp + sizeof sharp - 1, v.sharpness).ptr = '\0';
  std::string t = "# gow2 settings (written by the launcher; keys are described in the README).\n\n[video]\n";
  t += std::string("mode = \"") + kMode[int(v.mode)] + "\"\n";
  if (v.width) t += "width = " + std::to_string(v.width) + "\n";
  if (v.height) t += "height = " + std::to_string(v.height) + "\n";
  t += std::string("scaling = \"") + (v.scaling == Scaling::kFit ? "fit" : "stretch") + "\"\n";
  t += std::string("vsync = ") + (v.vsync ? "true" : "false") + "\n";
  t += std::string("resizable = ") + (v.resizable ? "true" : "false") + "\n";
  t += std::string("antialiasing = \"") + kAa[int(v.antialiasing)] + "\"\n";
  t += std::string("sharpening = ") + (v.sharpening ? "true" : "false") + "\n";
  t += std::string("sharpness = ") + sharp + "\n";
  t += "internal_resolution = " + std::to_string(v.internal_resolution) + "\n";
  t += "anisotropic = " + std::to_string(v.anisotropic) + "\n";
  t += std::string("enhanced_shaders = ") + (v.enhanced_shaders ? "true" : "false") + "\n";
  t += std::string("upscaler = \"") + kUp[int(v.upscaler)] + "\"\n";
  t += std::string("vrr = \"") + (v.vrr == Vrr::kAuto ? "auto" : v.vrr == Vrr::kOn ? "on" : "off") + "\"\n";
  t += std::string("frame_generation = \"") + std::string(FrameGenerationName(v.frame_generation)) + "\"\n";
  t += std::string("hdr = ") + (v.hdr ? "true" : "false") + "\n";
  t += "hdr_peak_nits = " + std::to_string(v.hdr_peak_nits) + "\n";
  t += "hdr_paper_white_nits = " + std::to_string(v.hdr_paper_white_nits) + "\n";
  t += std::string("aspect = \"") + std::string(AspectName(v.aspect)) + "\"\n";
  t += std::string("dlss_preset = \"") + std::string(DlssPresetName(v.dlss_preset)) + "\"\n";
  t += std::string("dlss_mode = \"") + std::string(DlssModeName(v.dlss_mode)) + "\"\n";
  t += "ui_scale = " + std::to_string(v.ui_scale) + "\n";
  t += "\n[patches]\n";
  for (const auto k : kPatchKeys)
    t += std::string(k) + " = " + (s.patches.count(std::string(k)) ? "true" : "false") + "\n";
  t += "\n[game]\n";
  const bool numeric = std::isdigit(static_cast<unsigned char>(FrameRateName(s.frame_rate)[0]));
  t += "frame_rate = " + (numeric ? std::string(FrameRateName(s.frame_rate)) : q(FrameRateName(s.frame_rate))) + "\n";
  if (!s.language.empty()) t += "language = " + q(s.language) + "\n";
  t += "\n[engine]\n";
  t += "shadow_quality = \"" + std::string(ShadowQualityName(s.engine.shadow_quality)) + "\"\n";
  t += std::string("dynamic_shadows = ") + (s.engine.dynamic_shadows ? "true" : "false") + "\n";
  if (s.console) t += "\n[debug]\nconsole = true\n";
  if (!s.gpu_cvars.empty()) {
    t += "\n[gpu]\ncvars = { ";
    for (size_t i = 0; i < s.gpu_cvars.size(); ++i)
      t += (i ? ", " : "") + q(s.gpu_cvars[i].first) + " = " + q(s.gpu_cvars[i].second);
    t += " }\n";
  }
  return t;
}

std::optional<std::string> SaveFile(const std::filesystem::path& path, const Settings& s) {
  const std::u8string u8 = path.filename().u8string();
  const std::string file(u8.begin(), u8.end());
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  const std::string text = Serialize(s);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return file + ": cannot write the file";
    out.write(text.data(), std::streamsize(text.size()));
    out.close();
    std::error_code ec;
    if (!out) {
      std::filesystem::remove(tmp, ec);
      return file + ": write failed";
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);  // MoveFileExW with replacement
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return file + ": " + ec.message();
  }
  return std::nullopt;
}

bool ParseResolution(std::string_view t, uint32_t& w, uint32_t& h) {
  const size_t x = t.find('x');
  if (x == std::string_view::npos) return false;
  const auto num = [](std::string_view d, uint32_t lo, uint32_t hi, uint32_t& v) {
    if (d.empty() || d.size() > 5) return false;
    uint32_t n = 0;
    for (char c : d) {
      if (c < '0' || c > '9') return false;
      n = n * 10 + uint32_t(c - '0');
    }
    if (n < lo || n > hi) return false;
    v = n;
    return true;
  };
  uint32_t a, b;
  if (!num(t.substr(0, x), kMinW, kMaxW, a) || !num(t.substr(x + 1), kMinH, kMaxH, b)) return false;
  w = a;
  h = b;
  return true;
}

std::optional<WindowMode> ParseWindowMode(std::string_view t) {
  if (t == "windowed") return WindowMode::kWindowed;
  if (t == "borderless") return WindowMode::kBorderless;
  if (t == "fullscreen") return WindowMode::kFullscreen;
  return std::nullopt;
}

std::string_view AnisotropicOverrideName(int32_t cvar) {
  static constexpr std::string_view kNames[] = {"off", "1x", "2x", "4x", "8x", "16x"};
  return cvar >= 0 && cvar <= 5 ? kNames[cvar] : "game default";
}

int32_t AnisotropicOverride(uint32_t a) {
  switch (a) {
    case 2: return 2;
    case 4: return 3;
    case 8: return 4;
    case 16: return 5;
    default: return -1;
  }
}

std::optional<Upscaler> ParseUpscaler(std::string_view t) {
  if (t == "off") return Upscaler::kOff;
  if (t == "fsr") return Upscaler::kFsr;
  if (t == "dlss") return Upscaler::kDlss;
  return std::nullopt;
}

std::optional<FrameRate> ParseFrameRate(std::string_view t) {
  for (const FrameRate r : {FrameRate::k30, FrameRate::k60, FrameRate::k120, FrameRate::k144, FrameRate::kMonitor,
                            FrameRate::kUnlimited})
    if (t == FrameRateName(r)) return r;
  return std::nullopt;
}

std::string_view FrameRateName(FrameRate r) {
  switch (r) {
    case FrameRate::k30: return "30";
    case FrameRate::k60: return "60";
    case FrameRate::k120: return "120";
    case FrameRate::k144: return "144";
    case FrameRate::kMonitor: return "monitor";
    case FrameRate::kUnlimited: return "unlimited";
  }
  return "30";
}

std::optional<Vrr> ParseVrr(std::string_view t) {
  if (t == "auto") return Vrr::kAuto;
  if (t == "on") return Vrr::kOn;
  if (t == "off") return Vrr::kOff;
  return std::nullopt;
}

std::optional<DlssPreset> ParseDlssPreset(std::string_view t) {
  for (const DlssPreset p : {DlssPreset::kDefault, DlssPreset::kE, DlssPreset::kF, DlssPreset::kJ, DlssPreset::kK,
                             DlssPreset::kL, DlssPreset::kM})
    if (t == DlssPresetName(p)) return p;
  return std::nullopt;
}

std::string_view DlssPresetName(DlssPreset p) {
  switch (p) {
    case DlssPreset::kDefault: return "default";
    case DlssPreset::kE: return "e";
    case DlssPreset::kF: return "f";
    case DlssPreset::kJ: return "j";
    case DlssPreset::kK: return "k";
    case DlssPreset::kL: return "l";
    case DlssPreset::kM: return "m";
  }
  return "default";
}

std::optional<FrameGeneration> ParseFrameGeneration(std::string_view t) {
  for (const FrameGeneration f : {FrameGeneration::kOff, FrameGeneration::kFsr, FrameGeneration::kDlss})
    if (t == FrameGenerationName(f)) return f;
  return std::nullopt;
}

std::string_view FrameGenerationName(FrameGeneration f) {
  switch (f) {
    case FrameGeneration::kOff: return "off";
    case FrameGeneration::kFsr: return "fsr";
    case FrameGeneration::kDlss: return "dlss";
  }
  return "off";
}

std::optional<DlssMode> ParseDlssMode(std::string_view t) {
  for (const DlssMode m : {DlssMode::kAuto, DlssMode::kDlaa, DlssMode::kQuality, DlssMode::kBalanced,
                           DlssMode::kPerformance, DlssMode::kUltraPerformance})
    if (t == DlssModeName(m)) return m;
  return std::nullopt;
}

std::string_view DlssModeName(DlssMode m) {
  switch (m) {
    case DlssMode::kAuto: return "auto";
    case DlssMode::kDlaa: return "dlaa";
    case DlssMode::kQuality: return "quality";
    case DlssMode::kBalanced: return "balanced";
    case DlssMode::kPerformance: return "performance";
    case DlssMode::kUltraPerformance: return "ultra_performance";
  }
  return "auto";
}

std::pair<uint32_t, uint32_t> OutputSizeFor(const Video& v, uint32_t desktop_w, uint32_t desktop_h) {
  const bool has_size = v.width != 0 && v.height != 0;
  switch (v.mode) {
    case WindowMode::kWindowed: return has_size ? std::pair{v.width, v.height} : std::pair{1280u, 720u};
    case WindowMode::kBorderless: return {desktop_w, desktop_h};
    case WindowMode::kFullscreen: return has_size ? std::pair{v.width, v.height} : std::pair{desktop_w, desktop_h};
  }
  return {desktop_w, desktop_h};
}

namespace {
uint32_t SceneHeight(Aspect a) { return a == Aspect::k21x9 ? 550 : a == Aspect::k16x10 ? 800 : 720; }
}  // namespace

uint32_t ImageWidthFor(const Video& v, uint32_t out_w, uint32_t out_h) {
  if (v.scaling == Scaling::kStretch) return out_w;
  return std::min<uint32_t>(out_w, uint32_t(uint64_t(out_h) * 1280 / SceneHeight(v.aspect)));
}

uint32_t ImageHeightFor(const Video& v, uint32_t out_w, uint32_t out_h) {
  if (v.scaling == Scaling::kStretch) return out_h;
  return std::min<uint32_t>(out_h, uint32_t(uint64_t(out_w) * SceneHeight(v.aspect) / 1280));
}

std::optional<uint32_t> InternalResolutionForDlssMode(DlssMode m, uint32_t image_w) {
  using Q = gow2_dlss::Quality;
  Q want;
  switch (m) {
    case DlssMode::kAuto: return std::nullopt;
    case DlssMode::kDlaa: want = Q::kDlaa; break;
    case DlssMode::kQuality: want = Q::kMaxQuality; break;
    case DlssMode::kBalanced: want = Q::kBalanced; break;
    case DlssMode::kPerformance: want = Q::kMaxPerf; break;
    case DlssMode::kUltraPerformance: want = Q::kUltraPerf; break;
    default: return std::nullopt;
  }
  for (uint32_t n = 1; n <= 3; ++n)
    if (gow2_dlss::QualityFor(1280 * n, image_w) == want) return n;
  return std::nullopt;
}

uint32_t EffectiveInternalResolution(const Video& v, uint32_t image_w) {
  if (v.upscaler != Upscaler::kDlss) return v.internal_resolution;
  return InternalResolutionForDlssMode(v.dlss_mode, image_w).value_or(v.internal_resolution);
}

std::optional<Aspect> ParseAspect(std::string_view t) {
  for (const Aspect a : {Aspect::k16x9, Aspect::k21x9, Aspect::k16x10})
    if (t == AspectName(a)) return a;
  return std::nullopt;
}

std::string_view AspectName(Aspect a) {
  switch (a) {
    case Aspect::k16x9: return "16:9";
    case Aspect::k21x9: return "21:9";
    case Aspect::k16x10: return "16:10";
  }
  return "16:9";
}

std::optional<ShadowQuality> ParseShadowQuality(std::string_view t) {
  for (const ShadowQuality q : {ShadowQuality::kLow, ShadowQuality::kOriginal, ShadowQuality::kHigh})
    if (t == ShadowQualityName(q)) return q;
  return std::nullopt;
}

std::string_view ShadowQualityName(ShadowQuality q) {
  switch (q) {
    case ShadowQuality::kLow: return "low";
    case ShadowQuality::kOriginal: return "original";
    case ShadowQuality::kHigh: return "high";
  }
  return "original";
}

std::optional<Antialiasing> ParseAntialiasing(std::string_view t) {
  if (t == "off") return Antialiasing::kOff;
  if (t == "fxaa") return Antialiasing::kFxaa;
  if (t == "fxaa_extreme") return Antialiasing::kFxaaExtreme;
  return std::nullopt;
}

}  // namespace settings
