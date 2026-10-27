#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

struct ExtractHooks {
  // Called after each 4 MiB chunk and once at the end with done == total (total = sum of file sizes).
  std::function<void(uint64_t done_bytes, uint64_t total_bytes)> progress;
  // Polled between chunks; returning true aborts the extraction.
  std::function<bool()> cancelled;
};

// Extracts every file of the ISO into out. Returns 0 ok, 1 format/read/write error, 3 cancelled.
// On success writes out/.extract_ok (file count); on error or cancellation the marker does not
// exist and the file being written is removed (already completed files stay).
// Overwrites existing files, deletes nothing.
int RunExtract(const std::filesystem::path& iso, const std::filesystem::path& out, std::ostream& log,
               const ExtractHooks& hooks = {});

// Contents of the root-level file `name`; nullopt if the ISO is not XDVDFS or the file is missing.
std::optional<std::vector<std::byte>> ReadIsoFile(const std::filesystem::path& iso, std::string_view name);

// Sum of the file sizes in the ISO; nullopt if the ISO is not XDVDFS.
std::optional<uint64_t> IsoPayloadBytes(const std::filesystem::path& iso);
