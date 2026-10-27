// Game data folder: default.xex edition check, extraction marker, folder resolution.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace game_data {

// SHA-256 of the supported default.xex (Gears of War 2, PAL Europe); from config/expected_xex.json.
inline constexpr std::string_view kExpectedXexSha256 = GOW2_XEX_SHA256;

// Lowercase hex; nullopt if the file cannot be read.
std::optional<std::string> Sha256File(const std::filesystem::path& p);
std::string Sha256Bytes(std::span<const std::byte> bytes);

enum class XexCheck { kOk, kMissing, kUnreadable, kMismatch };
// kOk also when GOW2_SKIP_XEX_HASH=1 (tests only) and the file exists.
XexCheck CheckXex(const std::filesystem::path& data_dir, std::string_view expected = kExpectedXexSha256);

// default.xex and the .extract_ok marker are both present.
bool ExtractionComplete(const std::filesystem::path& data_dir);

// ./game_data if it is a directory, otherwise <exe_dir>/game_data.
std::filesystem::path ResolveDataDir(const std::filesystem::path& exe_dir);

}  // namespace game_data
