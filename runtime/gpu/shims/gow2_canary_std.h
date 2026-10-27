// Force-included (/FI, cmake/XeniaGpu.cmake) in xenia_gpu_core and its users:
// code vendored from xenia-canary. In canary some base headers include
// submodule libraries (cxxopts and toml++ in cvar.h, microprofile in
// profiling.h) that bring in many standard library headers, and canary's code
// uses them without including them (std::array, std::unordered_set, ...). The
// shims do not include those libraries: this header provides the same standard headers.
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
