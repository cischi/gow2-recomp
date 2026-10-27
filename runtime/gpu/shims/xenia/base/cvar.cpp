// Shim of Xenia's src/xenia/base/cvar.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): cvar registry by name and value conversion.
#include "xenia/base/cvar.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>

namespace cvar {
namespace {

struct Registry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<IConfigVar>, std::less<>> vars;
};

// Function-local: cvars register during other modules' dynamic
// initialization, in unspecified order.
Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

template <typename T>
bool ParseNumber(std::string_view text, T& out) {
  if (text.empty()) {
    return false;
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (text.front() == '-' || text.front() == '+') {
      return false;
    }
  }
  T value{};
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

bool ParseValue(std::string_view text, bool& out) {
  if (text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0") {
    out = false;
    return true;
  }
  return false;
}
bool ParseValue(std::string_view text, int32_t& out) { return ParseNumber(text, out); }
bool ParseValue(std::string_view text, uint32_t& out) { return ParseNumber(text, out); }
bool ParseValue(std::string_view text, uint64_t& out) { return ParseNumber(text, out); }
bool ParseValue(std::string_view text, double& out) { return ParseNumber(text, out); }
bool ParseValue(std::string_view text, std::string& out) {
  out.assign(text);
  return true;
}
bool ParseValue(std::string_view text, std::filesystem::path& out) {
  // UTF-8 text (like Xenia's path cvars).
  out = std::filesystem::u8path(text.begin(), text.end());
  return true;
}

void Register(IConfigVar* var) {
  std::unique_ptr<IConfigVar> owned(var);
  Registry& registry = GetRegistry();
  std::lock_guard lock(registry.mutex);
  // A name defined twice is a link error in Xenia (same variable
  // cvars::name); here the first registration wins.
  registry.vars.try_emplace(owned->name(), std::move(owned));
}

}  // namespace cvar

namespace gow2::xenia {

// Choice-valued string cvars: Xenia treats an unknown value as the default (readback_resolve "=true" silently became
// "fast"), here it is rejected.
constexpr std::string_view kReadbackResolve[] = {"fast", "full", "none"};
constexpr std::string_view kOcclusionQuery[] = {"fake", "fast", "fast-alt", "strict"};
constexpr std::pair<std::string_view, std::span<const std::string_view>> kStringChoices[] = {
    {"readback_resolve", kReadbackResolve},
    {"occlusion_query", kOcclusionQuery},
};

bool SetCvar(std::string_view name, std::string_view value) {
  for (const auto& [cvar_name, choices] : kStringChoices)
    if (name == cvar_name && std::find(choices.begin(), choices.end(), value) == choices.end()) return false;
  cvar::Registry& registry = cvar::GetRegistry();
  std::lock_guard lock(registry.mutex);
  auto it = registry.vars.find(name);
  if (it == registry.vars.end()) {
    return false;
  }
  return it->second->SetFromString(value);
}

std::vector<std::string> CvarNames() {
  cvar::Registry& registry = cvar::GetRegistry();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> names;
  names.reserve(registry.vars.size());
  for (const auto& entry : registry.vars) {
    names.push_back(entry.first);
  }
  return names;
}

}  // namespace gow2::xenia
