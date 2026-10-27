#include "engine_config.h"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

namespace engine_config {
namespace {

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> b) : b_(b) {}
  size_t Remaining() const { return b_.size() - pos_; }
  bool I32(int32_t& v) {
    if (Remaining() < 4) return false;
    const uint32_t u = uint32_t(b_[pos_]) << 24 | uint32_t(b_[pos_ + 1]) << 16 | uint32_t(b_[pos_ + 2]) << 8 |
                       b_[pos_ + 3];
    v = static_cast<int32_t>(u);
    pos_ += 4;
    return true;
  }
  // Count of elements that take at least 4 bytes each.
  bool Count(size_t& n) {
    int32_t v;
    if (!I32(v) || v < 0 || size_t(v) > Remaining() / 4) return false;
    n = size_t(v);
    return true;
  }
  bool Str(FString& s) {
    int32_t n;
    if (!I32(n)) return false;
    s = FString{};
    if (n == 0) {
      s.zero = true;
      return true;
    }
    const int64_t chars = n < 0 ? -int64_t(n) : int64_t(n);
    const uint64_t bytes = uint64_t(chars) * (n < 0 ? 2 : 1);
    if (bytes > Remaining()) return false;
    const uint8_t* p = &b_[pos_];
    s.wide = n < 0;
    const size_t len = size_t(chars) - 1;
    s.text.resize(len);
    for (size_t i = 0; i < len; ++i)
      s.text[i] = s.wide ? char16_t(p[2 * i] | p[2 * i + 1] << 8) : char16_t(p[i]);
    const bool term_zero = s.wide ? (p[2 * len] == 0 && p[2 * len + 1] == 0) : p[len] == 0;
    pos_ += size_t(bytes);
    return term_zero;
  }

 private:
  std::span<const uint8_t> b_;
  size_t pos_ = 0;
};

void PutI32(std::vector<uint8_t>& o, int32_t v) {
  const uint32_t u = uint32_t(v);
  o.insert(o.end(), {uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u)});
}
void PutStr(std::vector<uint8_t>& o, const FString& s) {
  if (s.zero) return PutI32(o, 0);
  if (s.wide) {
    PutI32(o, -int32_t(s.text.size() + 1));
    for (char16_t c : s.text) o.insert(o.end(), {uint8_t(c & 0xFF), uint8_t(c >> 8)});
    o.insert(o.end(), {0, 0});
  } else {
    PutI32(o, int32_t(s.text.size() + 1));
    for (char16_t c : s.text) o.push_back(uint8_t(c));
    o.push_back(0);
  }
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Utf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}
std::string Ascii(const std::u16string& t) {
  std::string s;
  for (char16_t c : t) s.push_back(c < 0x80 ? char(c) : '?');
  return s;
}
bool EqNoCase(const std::u16string& a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] > 0x7F || std::tolower(static_cast<int>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

}  // namespace

std::optional<std::vector<IniFile>> Parse(std::span<const uint8_t> bytes) {
  Reader r(bytes);
  size_t nf;
  if (!r.Count(nf)) return std::nullopt;
  std::vector<IniFile> files;
  for (size_t i = 0; i < nf; ++i) {
    IniFile f;
    size_t ns;
    if (!r.Str(f.name) || !r.Count(ns)) return std::nullopt;
    for (size_t j = 0; j < ns; ++j) {
      Section s;
      size_t ne;
      if (!r.Str(s.name) || !r.Count(ne)) return std::nullopt;
      for (size_t k = 0; k < ne; ++k) {
        Entry e;
        if (!r.Str(e.key) || !r.Str(e.value)) return std::nullopt;
        s.entries.push_back(std::move(e));
      }
      f.sections.push_back(std::move(s));
    }
    files.push_back(std::move(f));
  }
  return files;
}

std::vector<uint8_t> Serialize(const std::vector<IniFile>& files) {
  std::vector<uint8_t> o;
  PutI32(o, int32_t(files.size()));
  for (const auto& f : files) {
    PutStr(o, f.name);
    PutI32(o, int32_t(f.sections.size()));
    for (const auto& s : f.sections) {
      PutStr(o, s.name);
      PutI32(o, int32_t(s.entries.size()));
      for (const auto& e : s.entries) {
        PutStr(o, e.key);
        PutStr(o, e.value);
      }
    }
  }
  return o;
}

std::vector<Edit> EditsFor(const settings::Engine& e) {
  std::vector<Edit> v;
  if (e.shadow_quality == settings::ShadowQuality::kLow) v.push_back({"MaxShadowResolution", "", "256"});
  if (e.shadow_quality == settings::ShadowQuality::kHigh) v.push_back({"MaxShadowResolution", "", "2048"});
  if (!e.dynamic_shadows) v.push_back({"DynamicShadows", "", "False"});
  return v;
}

std::expected<std::vector<std::string>, std::string> ApplyEdits(std::vector<IniFile>& files,
                                                                std::span<const Edit> edits) {
  std::vector<IniFile> work = files;
  std::vector<std::string> log;
  Section* sec = nullptr;
  for (auto& f : work) {
    if (!Lower(Ascii(f.name.text)).ends_with("xe-gearengine.ini")) continue;
    for (auto& s : f.sections)
      if (EqNoCase(s.name.text, "SystemSettings")) {
        sec = &s;
        break;
      }
    break;
  }
  for (const Edit& ed : edits) {
    const std::string label = std::string(ed.key) + (ed.field.empty() ? "" : " " + std::string(ed.field));
    const std::string missing = "Xe-GearEngine.ini [SystemSettings] " + label + " missing";
    Entry* ent = nullptr;
    if (sec)
      for (auto& e : sec->entries)
        if (EqNoCase(e.key.text, ed.key)) {
          ent = &e;
          break;
        }
    if (!ent) return std::unexpected(missing);
    FString& v = ent->value;
    const std::u16string neu(ed.value.begin(), ed.value.end());
    std::string old;
    if (ed.field.empty()) {
      old = Ascii(v.text);
      v.text = neu;
    } else {
      const std::u16string name(ed.field.begin(), ed.field.end());
      size_t start = std::u16string::npos;
      std::u16string lower = v.text;
      for (char16_t& c : lower)
        if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
      for (const char16_t* pre : {u"(", u","}) {
        std::u16string pat = pre + name + u"=";
        for (char16_t& c : pat)
          if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
        const size_t p = lower.find(pat);
        if (p != std::u16string::npos && (start == std::u16string::npos || p + pat.size() < start))
          start = p + pat.size();
      }
      if (start == std::u16string::npos) return std::unexpected(missing);
      size_t end = v.text.find_first_of(u",)", start);
      if (end == std::u16string::npos) end = v.text.size();
      old = Ascii(v.text.substr(start, end - start));
      v.text.replace(start, end - start, neu);
    }
    v.zero = false;
    log.push_back(label + " " + old + " -> " + std::string(ed.value));
  }
  files = std::move(work);
  return log;
}

Prepared Prepare(const std::filesystem::path& cooked_dir, const std::filesystem::path& out_dir,
                 const settings::Engine& e) {
  namespace fs = std::filesystem;
  Prepared p;
  const std::vector<Edit> edits = EditsFor(e);
  if (edits.empty()) return p;
  const auto t0 = std::chrono::steady_clock::now();

  std::vector<fs::path> found;
  std::error_code ec;
  for (fs::directory_iterator it(cooked_dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (!it->is_regular_file(ec2)) continue;
    const std::string n = Lower(Utf8(it->path().filename()));
    if (n.starts_with("coalesced_") && n.ends_with(".bin")) found.push_back(it->path());
  }
  if (ec) {
    p.log.push_back("[engine] " + Utf8(cooked_dir) + ": read failed, original");
    return p;
  }
  std::sort(found.begin(), found.end(),
            [](const fs::path& a, const fs::path& b) { return Utf8(a.filename()) < Utf8(b.filename()); });

  fs::create_directories(out_dir, ec);
  for (const fs::path& path : found) {
    const std::string name = Utf8(path.filename());
    const std::string pre = "[engine] " + name + ": ";
    std::vector<uint8_t> bytes;
    {
      std::ifstream in(path, std::ios::binary);
      if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (!in.is_open() || in.bad()) {
        p.log.push_back(pre + "read failed, original");
        continue;
      }
    }
    auto files = Parse(bytes);
    if (!files) {
      p.log.push_back(pre + "invalid format, original");
      continue;
    }
    auto changes = ApplyEdits(*files, edits);
    if (!changes) {
      p.log.push_back(pre + changes.error() + ", original");
      continue;
    }
    const std::vector<uint8_t> out = Serialize(*files);
    const fs::path dst = out_dir / name;
    fs::path tmp = dst;
    tmp += ".tmp";
    bool ok = false;
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
      o.close();
      ok = !o.fail();
    }
    if (ok) {
      fs::rename(tmp, dst, ec);
      ok = !ec;
    }
    if (!ok) {
      fs::remove(tmp, ec);
      p.log.push_back(pre + "writing " + name + " failed, original");
      continue;
    }
    std::string line = pre;
    for (size_t i = 0; i < changes->size(); ++i) line += (i ? ", " : "") + (*changes)[i];
    p.log.push_back(line);
    const std::u8string u8 = fs::absolute(dst).u8string();
    p.replacements.emplace_back(name, std::string(u8.begin(), u8.end()));
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  p.log.push_back(p.replacements.empty()
                      ? "[engine] no Coalesced modified: original configuration"
                      : "[engine] " + std::to_string(p.replacements.size()) + " Coalesced modified in " +
                            std::to_string(ms) + " ms");
  return p;
}

}  // namespace engine_config
