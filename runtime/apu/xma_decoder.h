// XMA decoder (sub-project 4): one Xenia xe::apu::XmaContext for each of the
// 320 slots of kernel::xma::ContextArray(), driven by the Kick, Lock and Clear
// registers the game writes (kernel::xma::SetRegisterListener), and a host
// worker "XMA Decoder" that decodes the enabled contexts.
//
// Modelled on Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/apu/xma_decoder.cc (WorkerThreadMain, WriteRegister,
// AllocateContext/ReleaseContext).
//  - Kick -> XmaContext::Enable() + work event; Lock -> Disable(), which
//    waits for a Work in progress on the same context; Clear -> Clear().
//    Bits of unallocated contexts are ignored (xma_kicks counts them anyway,
//    in kernel::xma::OnRegisterWrite).
//  - The worker scans the enabled contexts; after 500 idle loops it waits for
//    the event for at most 20 ms.
//  - Register 0x606 (CurrentContextIndex) stays 0.
//
// Locks (global order: kernel lock -> slot mutex -> XmaContext internal lock
// -> physwatch::GlobalLock -> Xenia GPU lock -> log):
//  - each slot has a mutex held by the worker for the whole Work and by
//    Kick/Lock/Clear/allocation/release: so Lock and ReleaseContext wait for
//    the Work in progress;
//  - the context listener is called by kernel::xma under its mutex
//    (kernel -> slot), the register listener on the guest thread that writes
//    the register without the kernel lock;
//  - Work writes to the guest physical memory: on a page watched by physwatch
//    the VEH takes GlobalLock while the worker holds the slot mutex. Whoever
//    holds GlobalLock (GPU/physwatch host code) never calls the XMA decoder,
//    so the wait is always short and cycle-free.
//  - Every XmaContext call that reads or writes the context's 64 bytes is
//    preceded and followed by std::atomic_thread_fence(seq_cst): the game
//    writes them without a lock.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

struct GuestMemory;

namespace apu {

// Decode callback for an enabled context (index 0..319), called by the
// worker with the slot mutex held. Replaceable only by tests; default:
// XmaContext::Work. true = work done.
using XmaWorkFn = std::function<bool(uint32_t index)>;

// xenia-canary XMA context: XmaContextNew (default, the one canary maintains) or
// XmaContextOld (--xma-decoder old, fallback if new sounds bad).
enum class XmaDecoderKind { kNew, kOld };
// Only "new" | "old" (lowercase).
std::optional<XmaDecoderKind> ParseXmaDecoder(std::string_view);
const char* XmaDecoderName(XmaDecoderKind);

// Creates one context per slot (on the first call, of type `kind`; later calls keep that one), installs
// the kernel::xma listeners and starts the worker. Must be called after kernel::xma::Init and before
// the game allocates contexts (already allocated contexts are not seen).
// false if kernel::xma is not initialized, if an XmaContext cannot be created or
// if a previous call used a different arena. A second call without
// Stop returns true with no effect.
bool StartXmaDecoder(GuestMemory&, XmaWorkFn work = {}, XmaDecoderKind kind = XmaDecoderKind::kNew);
// Removes the listeners and stops the worker (waiting for the Work in progress).
// Idempotent; returns within 100 ms. The XmaContexts remain (a listener running
// on a guest thread may still use them); a new Start restarts them free and
// disabled.
void StopXmaDecoder();

struct XmaContextState {
  bool allocated;
  bool enabled;
};
// Slot state (out of range or before Start: {false, false}).
XmaContextState GetXmaContextState(uint32_t index);

}  // namespace apu
