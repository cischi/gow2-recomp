// See input_script.h.
#include "hid/input_script.h"

namespace hid {
namespace {

struct ButtonName {
  std::string_view name;
  uint16_t bit;
};
constexpr ButtonName kButtons[] = {
    {"DPAD_UP", kButtonDpadUp},       {"DPAD_DOWN", kButtonDpadDown},   {"DPAD_LEFT", kButtonDpadLeft},
    {"DPAD_RIGHT", kButtonDpadRight}, {"START", kButtonStart},          {"BACK", kButtonBack},
    {"LTHUMB", kButtonLeftThumb},     {"RTHUMB", kButtonRightThumb},    {"LB", kButtonLeftShoulder},
    {"RB", kButtonRightShoulder},     {"A", kButtonA},                  {"B", kButtonB},
    {"X", kButtonX},                  {"Y", kButtonY}};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Decimal integer made only of digits (non-empty) <= max.
std::optional<uint32_t> ParseUInt(std::string_view s, uint32_t max) {
  if (s.empty()) return std::nullopt;
  uint64_t v = 0;
  for (char c : s) {
    if (!IsDigit(c)) return std::nullopt;
    v = v * 10 + uint32_t(c - '0');
    if (v > max) return std::nullopt;
  }
  return uint32_t(v);
}

// "<integer>[.<1..3 digits>]" seconds -> milliseconds, at most 86400 s.
std::optional<uint32_t> ParseSecondsToMs(std::string_view s) {
  const size_t dot = s.find('.');
  const auto whole = ParseUInt(s.substr(0, dot), 86400);
  if (!whole) return std::nullopt;
  uint32_t ms = *whole * 1000;
  if (dot != std::string_view::npos) {
    const std::string_view frac = s.substr(dot + 1);
    if (frac.empty() || frac.size() > 3) return std::nullopt;
    const auto f = ParseUInt(frac, 999);
    if (!f) return std::nullopt;
    uint32_t scaled = *f;
    for (size_t i = frac.size(); i < 3; ++i) scaled *= 10;
    ms += scaled;
  }
  if (ms > 86400u * 1000u) return std::nullopt;
  return ms;
}

std::optional<ScriptEvent> ParseEvent(std::string_view item) {
  const size_t colon = item.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const auto at = ParseSecondsToMs(item.substr(0, colon));
  if (!at) return std::nullopt;
  std::string_view rest = item.substr(colon + 1);
  uint32_t hold = 100;
  if (const size_t plus = rest.find('+'); plus != std::string_view::npos) {
    const auto h = ParseUInt(rest.substr(plus + 1), 60000);
    if (!h || *h == 0) return std::nullopt;
    hold = *h;
    rest = rest.substr(0, plus);
  }
  for (const auto& b : kButtons)
    if (b.name == rest) return ScriptEvent{*at, b.bit, hold};
  if (rest == "LS_UP") return ScriptEvent{*at, 0, hold, 0, 32767};
  if (rest == "LS_DOWN") return ScriptEvent{*at, 0, hold, 0, -32767};
  if (rest == "LS_LEFT") return ScriptEvent{*at, 0, hold, -32767, 0};
  if (rest == "LS_RIGHT") return ScriptEvent{*at, 0, hold, 32767, 0};
  if (rest == "LT") return ScriptEvent{*at, 0, hold, 0, 0, 255, 0};
  if (rest == "RT") return ScriptEvent{*at, 0, hold, 0, 0, 0, 255};
  return std::nullopt;
}

}  // namespace

std::optional<std::vector<ScriptEvent>> ParseInputScript(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::vector<ScriptEvent> events;
  while (true) {
    const size_t comma = s.find(',');
    const auto event = ParseEvent(s.substr(0, comma));
    if (!event) return std::nullopt;
    events.push_back(*event);
    if (comma == std::string_view::npos) break;
    s = s.substr(comma + 1);
  }
  return events;
}

}  // namespace hid
