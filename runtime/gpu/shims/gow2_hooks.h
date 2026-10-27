// gow2 hooks called by the patches to Xenia's vendored code
// (the draw-counters patch in thirdparty/xenia/patches): counters for the
// exit summary. Implemented in runtime/gpu/xenia_backend.cpp.
// Header with no runtime dependencies: included by Xenia's code (C++17).
#ifndef GOW2_HOOKS_H_
#define GOW2_HOOKS_H_

#include <cstdint>

namespace gow2::hooks {

// After IssueDraw in CommandProcessor::ExecutePacketType3Draw:
// kernel::stats::NoteGpuDraw (gpu_draws; gpu_draws_skipped if !succeeded).
void OnDraw(bool succeeded);
// Type 3 packet not executed (default case of ExecutePacketType3):
// kernel::stats::NoteGpuUnknownPacket and one log line per opcode.
void OnUnknownPacket(uint32_t opcode);
// After a draw's render targets are updated (RenderTargetCache::Update):
// kernel::stats::NoteGpuEdramTransfers (gpu_edram_transfers, patch 0017).
void OnEdramTransfers(uint32_t count);

}  // namespace gow2::hooks

#endif  // GOW2_HOOKS_H_
