// Fake GPU: backend of the common GPU module
// (gpu/gpu_system.h) without a renderer, selectable with --gpu fake.
//
// Modelled on Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/gpu/command_processor.cc (WorkerThreadMain,
// InitializeRingBuffer, EnableReadPointerWriteBack).
//
// Command processor: a guest thread with a host body (GuestThread::
// CreateHost, like Xenia's XHostThread) reads CP_RB_WPTR, executes the
// packets up to there (pm4.h) and writes the read pointer (big-endian)
// to the write-back address and to CP_RB_RPTR. The interrupts of
// PM4_INTERRUPT packets call gpu::DispatchInterrupt on this thread (source 1).
#pragma once

#include <cstdint>
#include <memory>

#include "gpu/gpu_system.h"

namespace gpu_fake {

// Fake GPU without presentation (--gpu fake).
std::unique_ptr<gpu::Backend> CreateBackend();

}  // namespace gpu_fake
