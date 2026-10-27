// Shim of Xenia's src/xenia/base/clock.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): only the host clock functions, on std::chrono. Guest
// time belongs to gow2's kernel, not to Xenia.
#ifndef XENIA_BASE_CLOCK_H_
#define XENIA_BASE_CLOCK_H_

#include <cstdint>

namespace xe {

class Clock {
 public:
  // Ticks of the host monotonic clock, in nanoseconds (frequency 1e9).
  static uint64_t QueryHostTickFrequency();
  static uint64_t QueryHostTickCount();
  // System time as FILETIME (100 ns intervals since 1601-01-01 UTC).
  static uint64_t QueryHostSystemTime();
  // Milliseconds since the clock was first used in the process.
  static uint64_t QueryHostUptimeMillis();
};

}  // namespace xe

#endif  // XENIA_BASE_CLOCK_H_
