// See screenshot.h.
#include "gpu/screenshot.h"

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

}  // namespace gpu
