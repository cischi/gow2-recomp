// Shim of Xenia's src/xenia/base/clock.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): host clock on std::chrono.
#include "xenia/base/clock.h"

#include <chrono>
#include <ratio>

namespace xe {
namespace {

std::chrono::steady_clock::time_point UptimeOrigin() {
  static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
  return origin;
}

// Distance between the FILETIME epoch (1601) and the Unix epoch (1970), in 100 ns.
constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;
using FileTimeUnits = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;

}  // namespace

uint64_t Clock::QueryHostTickFrequency() { return 1000000000ull; }

uint64_t Clock::QueryHostTickCount() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint64_t Clock::QueryHostSystemTime() {
  const auto since_unix =
      std::chrono::duration_cast<FileTimeUnits>(std::chrono::system_clock::now().time_since_epoch());
  return kFileTimeUnixEpoch + uint64_t(since_unix.count());
}

uint64_t Clock::QueryHostUptimeMillis() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - UptimeOrigin())
                      .count());
}

}  // namespace xe
