// GPU backend around Xenia's CommandProcessor (vendored in
// thirdparty/xenia with the gow2 patches): the command processor runs the game's
// ring with Xenia's code (PM4 packets, RegisterFile, draw, swap)
// on the common GPU module (gpu/gpu_system.h: guest registers, vblank,
// interrupts). The concrete GPU is the CommandProcessor subclass created
// by the factory: NullCommandProcessor in tests, D3D12 in the game.
//
// Bridge between the game and Xenia: xe::gpu::GraphicsSystem
// (runtime/gpu/shims/xenia/gpu/graphics_system.h). The command processor
// thread is a shim XHostThread = kernel::GuestThread::CreateHost
// ("GPU Commands"), so interrupts originate from a guest thread.
#pragma once

#include <functional>
#include <memory>

#include "gpu/gpu_system.h"

namespace xe::gpu {
class CommandProcessor;
class GraphicsSystem;
}  // namespace xe::gpu

namespace gpu {

using CommandProcessorFactory =
    std::function<std::unique_ptr<xe::gpu::CommandProcessor>(xe::gpu::GraphicsSystem*)>;

// The factory is called immediately, once: the CommandProcessor goes to the
// GraphicsSystem, which owns it. Start = CommandProcessor::Initialize
// (starts the thread), Stop = CommandProcessor::Shutdown (idempotent),
// OnVblank = increment_counter (Xenia MarkVblank). nullptr if the factory
// returns nullptr.
std::unique_ptr<Backend> CreateXeniaBackend(std::unique_ptr<xe::gpu::GraphicsSystem>, CommandProcessorFactory);

}  // namespace gpu
