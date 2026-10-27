// Shim of Xenia's src/xenia/base/logging.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): same interface used by the vendored GPU code
// (XELOGE/W/I/D/GPU/..., xe::FatalError), gow2 implementation.
//
// Every line goes to LogLine("[xenia] <c> <message>") (c = Xenia's prefix
// character: '!' error, 'w' warning, 'i' info, 'd' debug, 'G' GPU...).
// The XELOG* are macros instead of template functions: each has a static
// counter per call site and writes at most kMaxLinesPerCallSite lines, so
// an error per draw does not fill the log.
#ifndef XENIA_BASE_LOGGING_H_
#define XENIA_BASE_LOGGING_H_

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/fmt/include/fmt/std.h"  // like xenia-canary's logging.h
#include "xenia/base/string.h"

namespace xe {

#define XE_OPTION_ENABLE_LOGGING 1

enum class LogLevel {
  Error = 0,
  Warning,
  Info,
  Debug,
  Trace,
};

namespace logging {

constexpr char kPrefixCharError = '!';
constexpr char kPrefixCharWarning = 'w';
constexpr char kPrefixCharInfo = 'i';
constexpr char kPrefixCharDebug = 'd';

constexpr uint32_t kMaxLinesPerCallSite = 10;

// Writes "[xenia] <prefix_char> <str>" (without trailing newline) via LogLine.
void AppendLogLine(LogLevel log_level, char prefix_char, std::string_view str);

namespace internal {
// Xenia's fmt (pre-10) formatted enums as integers; fmt 11 does not: here they
// are passed as the underlying type's value. fmt 11 also rejects non-void
// pointers with {:p} (e.g. uint8_t* in xma_context.cc): they are passed as
// const void*. char pointers stay strings.
template <typename T>
decltype(auto) LogArg(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return static_cast<std::underlying_type_t<T>>(value);
  } else if constexpr (std::is_pointer_v<T> &&
                       !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char> &&
                       !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, wchar_t>) {
    return static_cast<const void*>(value);
  } else {
    return (value);
  }
}

}  // namespace internal

// xenia-canary: true if lines of this level go to the log (log_mask
// ignored). Here: errors, warnings and info, like the XELOG* written by the shim.
inline bool ShouldLog(LogLevel log_level, uint32_t /*log_mask*/ = 0) {
  return log_level <= LogLevel::Info;
}

// xenia-canary: lines accumulated and written together with submit(prefix).
template <LogLevel L>
struct LoggerBatch {
  std::string text;

  template <typename... Args>
  void operator()(const char* format, const Args&... args) {
    fmt::format_to(std::back_inserter(text), fmt::runtime(format), internal::LogArg(args)...);
  }
  void submit(char prefix_char) {
    if (ShouldLog(L) && !text.empty()) {
      AppendLogLine(L, prefix_char, text);
    }
    text.clear();
  }
};

namespace internal {
template <typename... Args>
void LogAtSite(std::atomic<uint32_t>& site_count, LogLevel log_level, char prefix_char,
               const char* format, const Args&... args) {
  if (site_count.fetch_add(1, std::memory_order_relaxed) >= kMaxLinesPerCallSite) {
    return;
  }
  AppendLogLine(log_level, prefix_char, fmt::format(fmt::runtime(format), LogArg(args)...));
}
}  // namespace internal

}  // namespace logging

// Unrecoverable error in Xenia code: line "[gpu] fatal: <str>", then
// the hook set with gow2::xenia::SetFatalErrorHook (if any) and exit with
// code 5 (GPU).
[[noreturn]] void FatalError(std::string_view str);

}  // namespace xe

namespace gow2::xenia {
// Called by xe::FatalError before exiting with code 5 (e.g. to write the
// summary). nullptr = no hook.
void SetFatalErrorHook(void (*hook)());
}  // namespace gow2::xenia

#define XE_SHIM_LOG_AT_SITE(level, prefix, ...)                                       \
  do {                                                                                \
    static ::std::atomic<uint32_t> xe_shim_log_site_count_{0};                       \
    ::xe::logging::internal::LogAtSite(xe_shim_log_site_count_, level, prefix,       \
                                       __VA_ARGS__);                                 \
  } while (false)

#define XELOGE(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Error, '!', __VA_ARGS__)
#define XELOGW(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Warning, 'w', __VA_ARGS__)
#define XELOGI(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'i', __VA_ARGS__)
#define XELOGD(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Debug, 'd', __VA_ARGS__)
#define XELOGCPU(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'C', __VA_ARGS__)
#define XELOGAPU(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'A', __VA_ARGS__)
#define XELOGGPU(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'G', __VA_ARGS__)
#define XELOGKERNEL(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'K', __VA_ARGS__)
#define XELOGFS(...) XE_SHIM_LOG_AT_SITE(::xe::LogLevel::Info, 'F', __VA_ARGS__)

#endif  // XENIA_BASE_LOGGING_H_
