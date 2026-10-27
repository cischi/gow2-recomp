// Definition of xenia-canary's src/xenia/base/platform_amd64.cc
// (https://github.com/xenia-canary/xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause) without xbyak (submodule not vendored) and without the
// x64_extension_mask cvar: direct CPUID, only the flags the vendored code
// reads (memory.cc: AVX2, PrefetchW, Movdir64M, FastRepMovs; shared_memory.cc:
// BMI1). The others stay 0.
#include "xenia/base/platform.h"

#include <intrin.h>

#include <cstdint>

namespace xe {
namespace amd64 {

static uint64_t DetectFeatureFlags() {
  int r[4];
  uint64_t flags = 0;
  __cpuid(r, 0);
  if (r[0] >= 7) {
    __cpuidex(r, 7, 0);
    if (r[1] & (1 << 5)) flags |= kX64EmitAVX2;
    if (r[1] & (1 << 3)) flags |= kX64EmitBMI1;
    if (r[1] & (1 << 9)) flags |= kX64FastRepMovs;
    if (r[2] & (1 << 28)) flags |= kX64EmitMovdir64M;
  }
  __cpuid(r, 0x80000000);
  if (static_cast<uint32_t>(r[0]) >= 0x80000001) {
    __cpuid(r, 0x80000001);
    if (r[2] & (1 << 8)) flags |= kX64EmitPrefetchW;
  }
  return flags;
}

uint64_t GetFeatureFlags() {
  static const uint64_t flags = DetectFeatureFlags();
  return flags;
}

void InitFeatureFlags() { GetFeatureFlags(); }

}  // namespace amd64
}  // namespace xe
