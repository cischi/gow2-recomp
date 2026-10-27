// gow2 hooks called by the patches thirdparty/xenia/patches/0007-gow2-xma-counters.patch and
// 0008-gow2-xma-empty-output.patch (xma_context_new.cc, xma_context_old.cc): summary counters
// printed at exit. Implemented in runtime/apu/xma_hooks.cpp (gow2_apu).
// Header with no runtime dependencies: it is included by Xenia code (C++17).
// Thread-safe: they are called by the XMA worker.
#ifndef GOW2_APU_HOOKS_H_
#define GOW2_APU_HOOKS_H_

#include <cstdint>

namespace gow2::hooks {

// After a successful avcodec_receive_frame in XmaContext::Decode:
// kernel::stats::NoteXmaFrameDecoded (xma_frames_decoded).
void OnXmaFrameDecoded();
// "Error sending packet" and "Error during decoding" branches of
// XmaContext::Decode: kernel::stats::NoteXmaDecodeError (xma_decode_errors)
// and, the first time per context, the line
// `[apu] XMA decode error context <id>`.
void OnXmaDecodeError(uint32_t context_id);

}  // namespace gow2::hooks

#endif  // GOW2_APU_HOOKS_H_
