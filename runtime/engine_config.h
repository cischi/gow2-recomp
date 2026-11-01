// Coalesced_*.bin (UE3, big endian): reading, byte-for-byte rewriting, editing of [SystemSettings].
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings.h"

namespace engine_config {

struct FString {
  std::u16string text;  // Latin-1 (wide == false) or UTF-16 (wide == true), without the trailing zero
  bool wide = true;
  bool zero = false;  // encoded as n == 0 (no terminator), distinct from "only the trailing zero"
  bool operator==(const FString&) const = default;
};
struct Entry {
  FString key, value;
  bool operator==(const Entry&) const = default;
};
struct Section {
  FString name;
  std::vector<Entry> entries;
  bool operator==(const Section&) const = default;
};
struct IniFile {
  FString name;
  std::vector<Section> sections;
  bool operator==(const IniFile&) const = default;
};

// nullopt if the bytes are not a Coalesced (bounds, negative counts or counts greater than the remaining bytes / 4).
// Bytes after the last file are ignored.
std::optional<std::vector<IniFile>> Parse(std::span<const uint8_t> bytes);
std::vector<uint8_t> Serialize(const std::vector<IniFile>& files);

// Edit requested by a non-default option. Empty field = the whole value; otherwise the
// field=... of the tuple "(A=..,B=..)".
struct Edit {
  std::string_view key, field, value;
};
// In the order shadow_quality, dynamic_shadows; empty with Engine{}.
std::vector<Edit> EditsFor(const settings::Engine& e);

// Applies `edits` to [SystemSettings] of the *-GearEngine.ini file (Xe-GearEngine.ini, Xbox360-GearEngine.ini;
// case-insensitive, first occurrence). All or nothing: on error `files` is left unchanged and the text is the reason.
// On success one entry per edit: "MaxShadowResolution 800 -> 2048".
std::expected<std::vector<std::string>, std::string> ApplyEdits(std::vector<IniFile>& files,
                                                                std::span<const Edit> edits);

struct Prepared {
  std::vector<std::pair<std::string, std::string>> replacements;  // file name -> absolute host path (UTF-8)
  std::vector<std::string> log;                                   // "[engine] ..." log lines
};
// Every Coalesced_*.bin (case-insensitive) directly in cooked_dir: read, edited, written
// to out_dir/<name> via <name>.tmp and a rename. out_dir is created if missing. Empty EditsFor(e): {}.
Prepared Prepare(const std::filesystem::path& cooked_dir, const std::filesystem::path& out_dir,
                 const settings::Engine& e);

}  // namespace engine_config
