// Unique temporary paths for tests. catch_discover_tests registers every
// TEST_CASE as a separate CTest test, and `ctest -j N` runs several processes of the
// same executable together: a fixed name per executable would make them write to
// (and delete) the same folder. The random suffix (64 bits from
// random_device, plus the time and a counter) tells processes and calls apart.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace gow2_test {

// <temp>/<tag>_<unique suffix>: not created.
inline std::filesystem::path UniqueTempPath(std::string_view tag) {
  static std::atomic<uint32_t> counter{0};
  std::random_device rd;
  const uint64_t r = (uint64_t(rd()) << 32) ^ rd() ^
                     uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  char suffix[48];
  std::snprintf(suffix, sizeof(suffix), "_%016llx_%u", static_cast<unsigned long long>(r),
                counter.fetch_add(1) + 1);
  return std::filesystem::temp_directory_path() / (std::string(tag) + suffix);
}

// Unique temporary folder, created immediately and deleted (with its contents)
// on destruction.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(std::string_view tag) : path(UniqueTempPath(tag)) {
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace gow2_test
