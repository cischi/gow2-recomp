// First-run game data setup: validation of the ISO the user picked, before extracting it.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <sstream>
#include <string>
#include <string_view>

#include "game_data.h"

namespace launcher {

enum class IsoCheck { kOk, kNotAnIso, kNoXex, kWrongEdition, kNoSpace };

// kNoSpace when free_bytes < payload size + 256 MiB (slack for the marker and filesystem overhead).
IsoCheck CheckIso(const std::filesystem::path& iso, uint64_t free_bytes, std::string_view expected);

// Extraction finished (marker present) and default.xex matches `expected`: Play is enabled.
bool DataReady(const std::filesystem::path& dir, std::string_view expected);

// Free bytes on the volume of `dir`, measured on its nearest existing ancestor; UINT64_MAX if it cannot be
// measured (the space check is then skipped rather than blaming the user).
uint64_t FreeSpaceFor(const std::filesystem::path& dir);

// ISO extraction on a worker thread. Members are declared so that the future (whose destructor waits for the
// thread) is destroyed before the state the thread uses.
struct Extraction {
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false};
  std::ostringstream log;
  std::future<int> result;
  bool running() const { return result.valid(); }
  void Start(const std::filesystem::path& iso, const std::filesystem::path& out);
  // Ready (or not running): the worker has finished.
  bool Finished() const;
  // Cancel and wait; returns the RunExtract exit code, or -1 if nothing was running.
  int CancelAndJoin();
  std::string LastLogLine() const;
};

}  // namespace launcher
