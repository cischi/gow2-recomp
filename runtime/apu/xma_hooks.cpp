// gow2::hooks glue from patches 0007/0008 (thirdparty/xenia/patches) into
// xenia-canary's APU (apu/shims/gow2_apu_hooks.h): summary counters and XMA
// decode error logging, at most one line per context.
#include "apu/shims/gow2_apu_hooks.h"

#include <array>
#include <atomic>
#include <cstdio>

#include "kernel/stats.h"
#include "kernel/xma.h"
#include "log.h"

namespace gow2::hooks {
namespace {

// One per context, plus one shared by out-of-range ids (they should not
// exist: only so the error is not lost).
using kernel::xma::kContextCount;
std::array<std::atomic<bool>, kContextCount + 1> g_error_logged{};

}  // namespace

void OnXmaFrameDecoded() { kernel::stats::NoteXmaFrameDecoded(); }

void OnXmaDecodeError(uint32_t context_id) {
  kernel::stats::NoteXmaDecodeError();
  const uint32_t slot = context_id < kContextCount ? context_id : kContextCount;
  if (g_error_logged[slot].exchange(true, std::memory_order_relaxed)) {
    return;
  }
  char line[64];
  std::snprintf(line, sizeof(line), "[apu] XMA decode error context %u", context_id);
  LogLine(line);
}

}  // namespace gow2::hooks
