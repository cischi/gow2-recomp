// Virtual file system (vfs.h). Xenia references
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/vfs/virtual_file_system.cc (VirtualFileSystem::ResolvePath,
// RegisterSymbolicLink, UnregisterSymbolicLink, FindSymbolicLink) and
// src/xenia/vfs/devices/host_path_entry.cc (child lookup). Code
// written from scratch; the security rules are our own (Review Focus 1).
#include "kernel/io/vfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace kernel::io {

namespace {

constexpr size_t kMaxPathLength = 1024;      // ANSI_STRING allows 64 KB; no game uses that many
constexpr size_t kMaxComponentLength = 255;  // the host's NAME_MAX
constexpr int kMaxLinkDepth = 8;             // links pointing to links

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = Lower(c);
  return out;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

// Separators unified to '\', no trailing '\' (except the string "\" itself).
std::string NormalizeSeparators(std::string_view s) {
  std::string out(s);
  std::replace(out.begin(), out.end(), '/', '\\');
  while (out.size() > 1 && out.back() == '\\') out.pop_back();
  return out;
}

std::string_view StripDosPrefix(std::string_view s) {
  if (s.size() >= 4 && s.substr(0, 4) == "\\??\\") s.remove_prefix(4);
  return s;
}

// Printable ASCII without the characters forbidden in file names. ':' is allowed
// here: it is checked per component, after the device.
bool ValidChars(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c >= 0x7F) return false;
    switch (c) {
      case '"':
      case '*':
      case '<':
      case '>':
      case '?':
      case '|':
        return false;
      default:
        break;
    }
  }
  return true;
}

// Windows device names: opening them would reach the console, the
// printer... even with an extension ("nul.txt").
bool IsReservedName(std::string_view comp) {
  std::string base = ToLower(comp.substr(0, comp.find('.')));
  while (!base.empty() && base.back() == ' ') base.pop_back();
  static constexpr std::array<std::string_view, 6> kFixed = {"con", "prn", "aux", "nul", "conin$", "conout$"};
  for (auto r : kFixed)
    if (base == r) return true;
  if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9')
    return true;
  return false;
}

bool ValidComponent(std::string_view comp) {
  if (comp.empty() || comp.size() > kMaxComponentLength) return false;
  if (comp.find(':') != std::string_view::npos) return false;  // C:, alternate streams
  if (comp.back() == '.' || comp.back() == ' ') return false;  // "...", ".. ", "a."
  return !IsReservedName(comp);
}

struct Link {
  std::string name;    // normalized, without "\??\"
  std::string target;  // normalized
};

struct MountEntry {
  Mount mount;           // normalized device
  fs::path canonical;    // absolute and canonical root
};

// Listing of a host directory: lowercase name -> host name.
struct Listing {
  std::unordered_map<std::string, fs::path> by_lower;
};

struct State {
  std::mutex mutex;
  std::vector<MountEntry> mounts;
  std::vector<Link> links;
  std::vector<std::pair<std::string, std::string>> replaced;  // lowercase name -> replacement name
  std::unordered_map<fs::path::string_type, std::shared_ptr<const Listing>> cache;
};

State& S() {
  static State s;
  return s;
}

fs::path CanonicalRoot(const fs::path& root) {
  std::error_code ec;
  fs::path abs = fs::absolute(root, ec);
  if (ec) abs = root;
  fs::path canon = fs::weakly_canonical(abs, ec);
  return (ec ? abs : canon).lexically_normal();
}

std::string LowerUtf8(const fs::path& name) {
  const auto u8 = name.u8string();
  return ToLower(std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
}

std::string Utf8(const fs::path& name) {
  const auto u8 = name.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Listing of `dir` (nullptr if it is not a readable directory). With the mutex held.
std::shared_ptr<const Listing> GetListing(State& s, const fs::path& dir, bool refresh) {
  if (!refresh) {
    auto it = s.cache.find(dir.native());
    if (it != s.cache.end()) return it->second;
  }
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    s.cache.erase(dir.native());
    return nullptr;
  }
  auto listing = std::make_shared<Listing>();
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path name = it->path().filename();
    listing->by_lower.emplace(LowerUtf8(name), name);
  }
  if (ec) return nullptr;
  s.cache[dir.native()] = listing;
  return listing;
}

// Longest prefix among `candidates` that matches `path` on a name
// boundary: end of string, '\' or, if the name ends with ':', anything.
template <class Range, class Name>
const auto* LongestPrefix(const Range& candidates, std::string_view path, Name name) {
  using Item = std::remove_reference_t<decltype(*std::begin(candidates))>;
  const Item* best = nullptr;
  size_t best_len = 0;
  for (const auto& c : candidates) {
    const std::string_view n = name(c);
    if (n.empty() || n.size() > path.size() || !EqualsNoCase(path.substr(0, n.size()), n)) continue;
    const bool boundary = path.size() == n.size() || path[n.size()] == '\\' || n.back() == ':';
    if (boundary && n.size() > best_len) {
      best = &c;
      best_len = n.size();
    }
  }
  return best;
}

// `child` lies under `root` (or coincides) after resolving the host's links and
// junctions.
bool InsideRoot(const fs::path& root_canonical, const fs::path& child) {
  std::error_code ec;
  const fs::path canon = fs::weakly_canonical(child, ec);
  if (ec) return false;
  const fs::path rel = canon.lexically_normal().lexically_relative(root_canonical);
  if (rel.empty()) return false;
  const auto first = rel.begin();
  return first == rel.end() || *first != "..";
}

}  // namespace

void SetMounts(std::vector<Mount> mounts) {
  std::vector<MountEntry> entries;
  for (auto& m : mounts) {
    m.device = NormalizeSeparators(StripDosPrefix(m.device));
    fs::path canon = CanonicalRoot(m.root);
    entries.push_back({std::move(m), std::move(canon)});
  }
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.mounts = std::move(entries);
  s.links.clear();
  s.cache.clear();
}

void AddMount(Mount m) {
  m.device = NormalizeSeparators(StripDosPrefix(m.device));
  fs::path canon = CanonicalRoot(m.root);
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::erase_if(s.mounts, [&](const MountEntry& e) { return EqualsNoCase(e.mount.device, m.device); });
  s.mounts.push_back({std::move(m), std::move(canon)});
}

bool RemoveMount(std::string_view device) {
  const std::string dev = NormalizeSeparators(StripDosPrefix(device));
  State& s = S();
  std::lock_guard lock(s.mutex);
  return std::erase_if(s.mounts, [&](const MountEntry& e) { return EqualsNoCase(e.mount.device, dev); }) > 0;
}

bool IsValidComponent(std::string_view name) {
  return name.find('\\') == std::string_view::npos && name.find('/') == std::string_view::npos &&
         ValidChars(name) && ValidComponent(name);
}

std::optional<Resolution> ResolveDetailed(std::string_view guest_path, ResolveError* error) {
  ResolveError dummy;
  ResolveError& err = error ? *error : dummy;
  err = ResolveError::kInvalidName;
  if (guest_path.empty()) {
    err = ResolveError::kUnknownDevice;
    return std::nullopt;
  }
  guest_path = StripDosPrefix(guest_path);  // "\??\" contains '?', which is forbidden elsewhere
  if (guest_path.size() > kMaxPathLength || !ValidChars(guest_path)) return std::nullopt;

  State& s = S();
  std::unique_lock lock(s.mutex);

  // Symbolic links (even nested, each at most once: "game:" ->
  // "game:\Sub" does not loop), then the mount with the longest prefix.
  std::string path = NormalizeSeparators(guest_path);
  const MountEntry* mount = nullptr;
  std::vector<const Link*> used;
  for (int depth = 0;; ++depth) {
    const Link* link = LongestPrefix(s.links, path, [&](const Link& l) -> std::string_view {
      return std::find(used.begin(), used.end(), &l) == used.end() ? std::string_view(l.name) : std::string_view();
    });
    const MountEntry* m =
        LongestPrefix(s.mounts, path, [](const MountEntry& e) -> std::string_view { return e.mount.device; });
    if (link && (!m || link->name.size() >= m->mount.device.size())) {
      if (depth >= kMaxLinkDepth) return std::nullopt;  // chain too long
      used.push_back(link);
      std::string rest = path.substr(link->name.size());
      if (!rest.empty() && rest.front() != '\\') rest.insert(rest.begin(), '\\');
      path = NormalizeSeparators(link->target + rest);
      if (path.size() > kMaxPathLength) return std::nullopt;
      continue;
    }
    if (!m) {
      err = ResolveError::kUnknownDevice;
      return std::nullopt;
    }
    mount = m;
    break;
  }

  // Components after the device: '.' ignored, '..' never above the root.
  std::vector<std::string_view> comps;
  const std::string_view rest = std::string_view(path).substr(mount->mount.device.size());
  for (size_t pos = 0; pos <= rest.size();) {
    size_t end = rest.find('\\', pos);
    if (end == std::string_view::npos) end = rest.size();
    const std::string_view comp = rest.substr(pos, end - pos);
    pos = end + 1;
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") {
      if (comps.empty()) return std::nullopt;
      comps.pop_back();
      continue;
    }
    if (!ValidComponent(comp)) return std::nullopt;
    comps.push_back(comp);
  }

  // Replaced files (--skip-movie): the last component becomes the replacement.
  std::string replaced_from;  // lowercase, if the last component is replaced (same folder)
  fs::path replaced_host;     // replacement with an absolute host path
  if (!comps.empty()) {
    const std::string last = ToLower(comps.back());
    for (const auto& [from, to] : s.replaced)
      if (from == last) {
        const fs::path abs(std::u8string(to.begin(), to.end()));
        if (abs.is_absolute()) {
          // only the game data (read-only mount): a namesake in cache: stays the game's file
          if (!mount->mount.writable) replaced_host = abs;
        } else {
          comps.back() = to;
          replaced_from = from;
        }
      }
  }

  // Case-insensitive search, one directory at a time.
  Resolution r;
  r.writable = mount->mount.writable;
  r.guest = mount->mount.device;
  r.host = mount->canonical;
  bool found = true;
  for (const std::string_view comp : comps) {
    fs::path name;
    if (found) {
      const std::string key = ToLower(comp);
      auto listing = GetListing(s, r.host, false);
      auto hit = listing ? listing->by_lower.find(key) : decltype(listing->by_lower.end()){};
      bool ok = listing && hit != listing->by_lower.end();
      if (!ok && r.writable) {  // someone may have created the file after the listing
        listing = GetListing(s, r.host, true);
        if (listing) {
          hit = listing->by_lower.find(key);
          ok = hit != listing->by_lower.end();
        }
      }
      if (ok) {
        name = hit->second;
      } else {
        found = false;
      }
    }
    if (name.empty()) name = fs::path(std::u8string(comp.begin(), comp.end()));
    r.host /= name;
    r.guest += '\\';
    r.guest += found ? Utf8(name) : std::string(comp);
  }
  r.exists = found;
  if (found && !replaced_from.empty()) {
    auto listing = GetListing(s, r.host.parent_path(), false);
    auto hit = listing ? listing->by_lower.find(replaced_from) : decltype(listing->by_lower.end()){};
    std::error_code ec;
    if (listing && hit != listing->by_lower.end()) {
      const uint64_t size = fs::file_size(r.host.parent_path() / hit->second, ec);
      if (!ec) r.min_size = size;
    }
  }
  const fs::path root = mount->canonical;
  lock.unlock();  // weakly_canonical opens files: outside the lock

  // Host links and junctions: the real path must stay under the root.
  if (!InsideRoot(root, r.host)) return std::nullopt;
  // Absolute replacement (engine Coalesced, chosen by the runtime): outside the mount, never writable.
  if (r.exists && !replaced_host.empty()) {
    std::error_code size_ec;
    const uint64_t size = fs::file_size(r.host, size_ec);
    if (!size_ec) r.min_size = size;
    r.host = std::move(replaced_host);
    r.writable = false;
  }
  err = ResolveError::kNone;
  return r;
}

void SetFileReplacements(std::vector<std::pair<std::string, std::string>> from_to) {
  for (auto& [from, to] : from_to) from = ToLower(from);
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.replaced = std::move(from_to);
}

std::optional<fs::path> Resolve(std::string_view guest_path) {
  auto r = ResolveDetailed(guest_path);
  if (!r) return std::nullopt;
  return std::move(r->host);
}

bool AddSymbolicLink(std::string_view link, std::string_view target) {
  link = StripDosPrefix(link);
  target = StripDosPrefix(target);
  if (!ValidChars(link) || !ValidChars(target) || link.size() > kMaxPathLength || target.size() > kMaxPathLength)
    return false;
  std::string name = NormalizeSeparators(StripDosPrefix(link));
  std::string to = NormalizeSeparators(StripDosPrefix(target));
  if (name.empty() || name == "\\" || to.empty()) return false;
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::erase_if(s.links, [&](const Link& l) { return EqualsNoCase(l.name, name); });
  s.links.push_back({std::move(name), std::move(to)});
  return true;
}

bool RemoveSymbolicLink(std::string_view link) {
  link = StripDosPrefix(link);
  if (!ValidChars(link)) return false;
  const std::string name = NormalizeSeparators(StripDosPrefix(link));
  State& s = S();
  std::lock_guard lock(s.mutex);
  return std::erase_if(s.links, [&](const Link& l) { return EqualsNoCase(l.name, name); }) > 0;
}

void InvalidateDirectory(const fs::path& host_dir) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.cache.erase(host_dir.native());
}

void InvalidateAllDirectories() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.cache.clear();
}

bool IsPackagePath(std::string_view guest_path) {
  const size_t sep = guest_path.find_last_of("\\/");
  const std::string_view name = sep == std::string_view::npos ? guest_path : guest_path.substr(sep + 1);
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string ext = ToLower(name.substr(dot));
  return ext == ".xxx" || ext == ".upk" || ext == ".tfc";
}

bool MatchPattern(std::string_view pattern, std::string_view name) {
  if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
  // Classic wildcard with backtracking on the last '*'.
  size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || Lower(pattern[p]) == Lower(name[n]))) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace kernel::io
