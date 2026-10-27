// Counters for the summary at gow2.exe's exit (`[summary] ...` lines).
// All functions are thread-safe.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace kernel::stats {

// Call to an import with no implementation: always counted; the first time per
// name it writes to the log `[kernel] UNIMPLEMENTED Name(r3=0x…, …, r10=0x…)` (the generated
// stubs pass the registers) or just `[kernel] UNIMPLEMENTED Name`.
// Counter of an unimplemented import, static in the generated stub
// (tools/gen_import_stubs.py). `name` must live as long as the process.
struct UnimplementedSlot {
  const char* name;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint32_t> state{0};  // 0 new, 1 registering, 2 registered
};
// Hot path of the stubs: one fetch_add, no lock and no
// allocation. The first call per slot registers it (mutex) for the
// summary and logs like the by-name overload; counts of the same name
// from the two overloads are summed in the summary.
void NoteUnimplemented(UnimplementedSlot&, std::span<const uint64_t, 8> r3_to_r10);
// By name: mutex and std::string on every call, for non-hot uses.
void NoteUnimplemented(const char* name);
void NoteUnimplemented(const char* name, std::span<const uint64_t, 8> r3_to_r10);
// Opening of a game package (guest path).
void NotePackageOpened(std::string_view guest_path);
void NoteFramePresented();
void NoteThreadCreated();
// Type 3 PM4 packet not executed by the fake GPU (opcode 0..0x7F;
// other values are ignored).
void NoteGpuUnknownPacket(uint32_t opcode);
// Xenia's GPU (runtime/gpu, gow2::hooks): a draw executed by the command
// processor (ok = false if the backend dropped it: it also counts among the
// skipped), a resolve (copy from EDRAM), a frame presented by the host
// (swap chain). Atomic, no lock.
void NoteGpuDraw(bool ok);
void NoteGpuResolve();
// EDRAM ownership transfers performed by a draw (Xenia render target cache, patch 0017).
void NoteGpuEdramTransfers(uint64_t count);
void NoteHostFramePresented();
// Audio, XMA and input (sub-project 4). Atomic, no lock.
//  - NoteAudioCallback: one call of a game audio client's callback;
//  - NoteAudioFrameSubmitted: one frame from XAudioSubmitRenderDriverFrame;
//  - NoteXmaKicks: write of a Kick register; `contexts` = contexts started (bits set to 1);
//  - NoteXmaFrameDecoded / NoteXmaDecodeError: outcome of the XMA decoder;
//  - NoteInputGetState: one call of XamInputGetState;
//  - NoteInputInjectedEvent: one --input-script event applied (diagnostics).
void NoteAudioCallback();
void NoteAudioFrameSubmitted();
void NoteXmaKicks(uint32_t contexts);
void NoteXmaFrameDecoded();
void NoteXmaDecodeError();
void NoteInputGetState();
void NoteInputInjectedEvent();

// Summary text, one `[summary] …` line per entry, each ending with '\n':
//   unimplemented <Name>=<calls>      (by descending calls, then by name)
//   gpu_unknown_packet 0x<OP>=<n>     (PM4 packets not executed, by descending n, then by opcode)
//   packages_opened=<n>
//   package <path>                    (the first 10 opened)
//   last_package <path>               (the last one opened; absent if none)
//   cooked_packages_opened=<n>        (.xxx under GearGame/CookedXenon, any case and separators)
//   frames_presented=<n>
//   gpu_draws=<n>
//   gpu_draws_skipped=<n>
//   gpu_resolves=<n>
//   gpu_edram_transfers=<n>
//   host_frames_presented=<n>
//   audio_callbacks=<n>
//   audio_frames_submitted=<n>
//   xma_kicks=<n>
//   xma_frames_decoded=<n>
//   xma_decode_errors=<n>
//   input_get_state_calls=<n>
//   input_injected_events=<n>
//   threads_created=<n>
std::string Summary();

// Writes Summary() to the log, one line per entry, at most once per process:
// it is called by every orderly exit (--run-seconds, entry returned,
// HalReturnToFirmware, KeBugCheck) and if two threads arrive together the second
// waits for the first to finish writing (then it may call _Exit without
// truncating the summary). Must not be called from the crash handler (it takes locks and
// allocates). Returns true if it did the writing.
bool WriteSummary();
// Function called by WriteSummary once, by the first caller, before
// composing the summary (main: apu::Shutdown, which stops the Audio Worker and
// closes the WAV capture on the kernel exits too, HalReturnToFirmware and
// KeBugCheck). Runs with the summary write lock held: it must not
// wait without limit for a thread that might call WriteSummary.
void SetBeforeSummaryHook(void (*hook)());

// --trace: the import stubs log every call.
void SetTrace(bool);
bool Trace();

}  // namespace kernel::stats
