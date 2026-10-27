// Shim of Xenia's src/xenia/base/profiling.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): the profiling macros are empty (as in Xenia with
// XE_OPTION_PROFILING = 0) and Profiler has only the methods without a
// graphical interface, all empty. No microprofile.
#ifndef XENIA_BASE_PROFILING_H_
#define XENIA_BASE_PROFILING_H_

#include <cstdint>

#define XE_OPTION_PROFILING 0
#define XE_OPTION_PROFILING_UI 0

#define DEFINE_profile_cpu(name, group_name, scope_name)
#define DEFINE_profile_gpu(name, group_name, scope_name)
#define DECLARE_profile_cpu(name)
#define DECLARE_profile_gpu(name)
#define SCOPE_profile_cpu(name) \
  do {                          \
  } while (false)
#define SCOPE_profile_cpu_f(name) \
  do {                            \
  } while (false)
#define SCOPE_profile_cpu_i(group_name, scope_name) \
  do {                                              \
  } while (false)
#define SCOPE_profile_gpu(name) \
  do {                          \
  } while (false)
#define SCOPE_profile_gpu_f(name) \
  do {                            \
  } while (false)
#define SCOPE_profile_gpu_i(group_name, scope_name) \
  do {                                              \
  } while (false)
#define COUNT_profile_add(name, count) \
  do {                                 \
  } while (false)
#define COUNT_profile_sub(name, count) \
  do {                                 \
  } while (false)
#define COUNT_profile_set(name, count) \
  do {                                 \
  } while (false)
#define COUNT_profile_cpu(name, count) \
  do {                                 \
  } while (false)
#define COUNT_profile_gpu(name, count) \
  do {                                 \
  } while (false)

namespace xe {

class Profiler {
 public:
  static bool is_enabled();
  static bool is_visible();
  static void Initialize();
  static void Dump();
  static void Shutdown();
  static void ThreadEnter(const char* name = nullptr);
  static void ThreadExit();
  static void Flip();
};

}  // namespace xe

#endif  // XENIA_BASE_PROFILING_H_
