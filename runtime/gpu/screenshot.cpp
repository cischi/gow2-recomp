// See screenshot.h.
#include "gpu/screenshot.h"

#include <cstdio>
#include <string>

namespace gpu {

std::optional<ScreenshotRequest> ParseScreenshotArg(std::wstring_view arg) {
  const size_t colon = arg.find(L':');
  if (colon == std::wstring_view::npos || colon == 0 || colon + 1 == arg.size()) return std::nullopt;
  uint64_t seconds = 0;
  for (wchar_t c : arg.substr(0, colon)) {
    if (c < L'0' || c > L'9') return std::nullopt;
    seconds = seconds * 10 + uint32_t(c - L'0');
    if (seconds > 86400) return std::nullopt;
  }
  return ScreenshotRequest{uint32_t(seconds), std::filesystem::path(arg.substr(colon + 1))};
}

std::filesystem::path HotkeyScreenshotPath(const std::filesystem::path& dir, std::string_view game_id, const std::tm& t,
                                           unsigned ms) {
  char stamp[40];
  std::snprintf(stamp, sizeof stamp, "_%04d-%02d-%02d_%02d-%02d-%02d-%03u.bmp", t.tm_year + 1900, t.tm_mon + 1,
                t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms % 1000);
  const std::string id(game_id);
  return dir / "screenshots" / id / (id + stamp);
}

}  // namespace gpu
