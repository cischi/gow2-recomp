// Audio system: render clients of the game's XAudio driver, a guest thread
// "Audio Worker" that calls their callbacks paced by the credits the sink
// (plat::AudioOut) returns, frame conversion and WAV capture.
//
// Modelled on Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/apu/audio_system.cc (RegisterClient, UnregisterClient,
// SubmitFrame, WorkerThreadMain) and src/xenia/kernel/xboxkrnl/
// xboxkrnl_audio.cc (XAudio*RenderDriver*):
//  - at most 8 clients, handle 0x41550000 | index;
//  - RegisterClient reads callback = callback_ptr[0] and arg = callback_ptr[1]
//    (big-endian), allocates 4 bytes from the system heap, writes arg there
//    and passes that pointer to the callback in r3 ("wrapped argument");
//  - each client starts with 64 credits (Xenia's kMaximumQueuedFrames, the
//    per-client semaphore): the worker consumes one credit per callback, the
//    sink returns one for every consumed buffer (BufferDone). A callback that
//    submits no frames therefore stops after 64 calls.
//
// Concurrency. One mutex protects the client state; the game callback runs
// OUTSIDE the mutex (it may call SubmitFrame, Unregister, any export: no
// inversion with the kernel locks nor with physical_watch's GlobalLock).
// Writes to guest memory (handle, argument) and the destruction of sinks
// (which stops their thread, the source of BufferDone) happen with the mutex
// released. BufferDone does not take the mutex (per-slot atomic counter
// carrying the generation, worker wake-up through atomic wait/notify):
// SubmitFrame calls the sink's Submit (XAudio2 SubmitSourceBuffer) while
// holding the mutex, and the XAudio2 engine thread that calls
// OnBufferEnd -> BufferDone must never wait for it.
//
// Lifecycle (Review Focus 1: the game unregisters and registers again,
// sub_82956CD0):
//  - UnregisterClient does not return while the worker is inside that
//    client's callback (unless the call comes from the callback itself); after
//    it returns the callback is no longer called;
//  - credits belong to the registration: a BufferDone from a sink of a
//    previous registration is ignored (generation number), a new registration
//    in the same slot starts again from 64;
//  - the 4 bytes of the argument are freed only outside the callback (if the
//    unregistration comes from the callback, when it returns);
//  - Shutdown waits for the worker for at most kShutdownWaitMs: if the game
//    callback is blocked, the orderly exit (--run-seconds) proceeds anyway.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "apu/xma_decoder.h"
#include "platform/platform.h"

namespace apu {

inline constexpr uint32_t kMaxClients = 8;
// Queued 256-sample frames per client: the game always fills all the credits, so this is the audio
// latency. 64 (Xenia's kMaximumQueuedFrames) = 341 ms, gunshots were heard late; 16 = 85 ms, the
// minimum of xenia-canary's apu_max_queued_frames.
inline constexpr uint32_t kClientCredits = 16;
inline constexpr uint32_t kDriverTag = 0x41550000;    // handle = kDriverTag | index
inline constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
inline constexpr uint32_t kShutdownWaitMs = 1000;
// UnregisterClient waits for the running callback without limit; after this
// time it writes `[audio] Unregister waiting for the callback (client <i>)` once.
inline constexpr uint32_t kUnregisterLogMs = 2000;

// 6x256 planar BE floats (guest) -> interleaved LE: out[i*6 + c] = in[c*256 + i].
void ConvertFrame(const uint8_t* guest_be_planar, std::span<float, plat::kAudioFrameFloats> out);

// Only "xaudio2" | "null" (lowercase).
std::optional<plat::AudioSinkKind> ParseAudioSink(std::string_view);

struct AudioOptions {
  plat::AudioSinkKind sink = plat::AudioSinkKind::kXAudio2;
  bool mute = false;
  std::optional<std::filesystem::path> capture;
  XmaDecoderKind xma_decoder = XmaDecoderKind::kNew;  // --xma-decoder (apu/xma_decoder.h)
};

using SinkFactory = std::function<std::unique_ptr<plat::AudioOut>(plat::AudioSinkKind, plat::AudioOut::BufferDone)>;

// Must be called after kernel::InitMemory and kernel::InitThreads. {} = plat::
// sinks: with kXAudio2, plat::CreateXAudio2AudioOut for every client and, if
// it returns nullptr, the line `[audio] xaudio2 not available (hr=0x%08X),
// using the null sink` and plat::CreateNullAudioOut. A critical error midway
// is handled by the sink itself (internal null pacing).
// false if the capture file cannot be opened. A second call without
// Shutdown returns true with no effect.
bool Init(const AudioOptions&, SinkFactory = {});
// Stops the worker (waiting for it at most kShutdownWaitMs), destroys the
// sinks and closes the capture. Idempotent; Init can be called again after
// Shutdown.
void Shutdown();

// Export implementations (Xenia's xboxkrnl_audio.cc).
// 0 or kStatusUnsuccessful (8 clients in use, system not initialized).
uint32_t RegisterClient(uint32_t callback_ptr, uint32_t driver_handle_out);
uint32_t UnregisterClient(uint32_t handle);
uint32_t SubmitFrame(uint32_t handle, uint32_t samples_ptr);

// Samples per channel written to the capture (0 without a capture).
uint64_t CaptureSampleFramesWritten();

}  // namespace apu
