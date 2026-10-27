// Shim of Xenia's src/xenia/base/logging.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): Xenia's log lines end up in gow2's log.
#include "xenia/base/logging.h"

#include <atomic>
#include <cstdlib>
#include <string>

#include "log.h"

namespace {
std::atomic<void (*)()> g_fatal_error_hook{nullptr};
}  // namespace

namespace xe {
namespace logging {

void AppendLogLine(LogLevel /*log_level*/, char prefix_char, std::string_view str) {
  while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
    str.remove_suffix(1);
  }
  std::string line = "[xenia] ";
  line += prefix_char;
  line += ' ';
  line += str;
  LogLine(line);
}

}  // namespace logging

void FatalError(std::string_view str) {
  std::string line = "[gpu] fatal: ";
  line += str;
  LogLine(line);
  if (void (*hook)() = g_fatal_error_hook.load()) {
    hook();
  }
  // Like the kernel's WriteSummaryAndExit: LogLine has already flushed, and no
  // static destructors run from an arbitrary thread.
  std::_Exit(5);
}

}  // namespace xe

namespace gow2::xenia {
void SetFatalErrorHook(void (*hook)()) { g_fatal_error_hook.store(hook); }
}  // namespace gow2::xenia
