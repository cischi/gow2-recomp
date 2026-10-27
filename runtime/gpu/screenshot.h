// gow2 screenshots (--screenshot S:file, repeatable): argument and request. The
// capture (back buffer -> BMP) is done by the presenter (gpu/presenter.h).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace gpu {

// First frame presented after `seconds` seconds from startup, saved to `file`.
struct ScreenshotRequest {
  uint32_t seconds;
  std::filesystem::path file;
};

// "<seconds>:<file>": decimal integer seconds 0..86400 (digits only), non-empty
// file; split at the first ':' (the file may contain one, e.g. "C:\x.bmp").
// nullopt if malformed.
std::optional<ScreenshotRequest> ParseScreenshotArg(std::wstring_view);

}  // namespace gpu
