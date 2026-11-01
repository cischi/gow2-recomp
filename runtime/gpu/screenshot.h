// gow2 screenshots (--screenshot S:file, repeatable, and F12 in game): argument,
// request and file name. The capture (back buffer -> BMP) is done by the
// presenter (gpu/presenter.h).
#pragma once

#include <cstdint>
#include <ctime>
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

// F12 screenshot of game `game_id` (game_profile id, "gow2"/"gow3") taken at local time `t`, millisecond `ms`:
// <dir>/screenshots/<game_id>/<game_id>_YYYY-MM-DD_HH-MM-SS-mmm.bmp.
std::filesystem::path HotkeyScreenshotPath(const std::filesystem::path& dir, std::string_view game_id, const std::tm& t,
                                           unsigned ms);

}  // namespace gpu
