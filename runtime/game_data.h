// Game data folder: default.xex edition check, extraction marker, folder resolution.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace game_data {

// Lowercase hex; nullopt if the file cannot be read.
std::optional<std::string> Sha256File(const std::filesystem::path& p);
std::string Sha256Bytes(std::span<const std::byte> bytes);

// SHA-256 of the decrypted, decompressed image of an XEX (as xexdump writes image.bin): the same code under a
// different container (region, media ID, XEX version) gives the same hash. nullopt if it is not a valid XEX.
std::optional<std::string> XexImageSha256(std::span<const std::byte> xex);

enum class XexCheck { kOk, kMissing, kUnreadable, kMismatch };
// Compares XexImageSha256 of default.xex with `expected` (an invalid XEX is kMismatch).
// kOk also when GOW2_SKIP_XEX_HASH=1 (tests only) and the file exists.
XexCheck CheckXex(const std::filesystem::path& data_dir, std::string_view expected);

// default.xex and the .extract_ok marker are both present.
bool ExtractionComplete(const std::filesystem::path& data_dir);

// ./<dir_name> if it is a directory, otherwise <exe_dir>/<dir_name>.
std::filesystem::path ResolveDataDir(const std::filesystem::path& exe_dir, std::string_view dir_name);

}  // namespace game_data
