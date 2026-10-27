// Shim of Xenia's src/xenia/base/profiling.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): profiler off, empty methods.
#include "xenia/base/profiling.h"

namespace xe {

bool Profiler::is_enabled() { return false; }
bool Profiler::is_visible() { return false; }
void Profiler::Initialize() {}
void Profiler::Dump() {}
void Profiler::Shutdown() {}
void Profiler::ThreadEnter(const char* /*name*/) {}
void Profiler::ThreadExit() {}
void Profiler::Flip() {}

}  // namespace xe
