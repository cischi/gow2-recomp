// Shim of Xenia's src/xenia/gpu/graphics_system.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): see graphics_system.h. Builds with the runtime (library
// gow2_gpu_xenia, C++23): registers and interrupts go through the common GPU module.
#include "xenia/gpu/graphics_system.h"

#include <cstdio>
#include <utility>

#include "gpu/gpu_system.h"
#include "gpu/xenos.h"
#include "log.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/command_processor.h"

namespace xe {
namespace gpu {

GraphicsSystem::GraphicsSystem(Memory* memory, ui::GraphicsProvider* provider, ui::Presenter* presenter)
    : memory_(memory), provider_(provider), presenter_(presenter), kernel_state_(memory) {}

GraphicsSystem::~GraphicsSystem() {
  // The CommandProcessor uses the RegisterFile: it dies first.
  command_processor_.reset();
}

void GraphicsSystem::gow2_SetCommandProcessor(std::unique_ptr<CommandProcessor> command_processor) {
  command_processor_ = std::move(command_processor);
}

// Xenia GraphicsSystem::InitializeRingBuffer -> CommandProcessor. The flag
// (release) publishes the ring to the command processor thread, which reads it
// (acquire) in gow2_PollWritePointer before using the write pointer.
// A second initialization while the CP runs has the same race as Xenia
// (the game only does it at startup).
void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  if (!command_processor_) return;
  command_processor_->InitializeRingBuffer(ptr, size_log2);
  ring_initialized_.store(true, std::memory_order_release);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  if (command_processor_) command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  ::gpu::DispatchInterrupt(source, cpu);
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(bool is_responsible) {
  // Only once: the first caller writes the line and exits (exit 5).
  if (host_gpu_loss_reported_.test_and_set()) return;
  ::gpu::LogDeviceRemovedOnce(uint32_t(gow2_DeviceRemovedReason()));
  xe::FatalError(is_responsible ? "D3D12 device lost (caused by the command processor)" : "D3D12 device lost");
}

// Xenia: GraphicsSystem::WriteRegister(CP_RB_WPTR) -> UpdateWritePointer.
// Here the CPU writes to memory: the command processor re-reads it on every loop.
uint32_t GraphicsSystem::gow2_PollWritePointer() const {
  if (!ring_initialized_.load(std::memory_order_acquire)) return 0xBAADF00D;  // "no write pointer" (Xenia)
  return ::gpu::ReadRegister(::gpu::kRegCpRbWptr);
}

void GraphicsSystem::gow2_MirrorRegister(uint32_t reg, uint32_t value) { ::gpu::WriteRegister(reg, value); }

uint32_t GraphicsSystem::gow2_ReadRegisterForWait(uint32_t reg) const {
  if (reg == XE_GPU_REG_COHER_STATUS_HOST) {
    // MakeCoherent clears it in the RegisterFile (volatile as in Xenia) without
    // going through WriteRegister: it is copied here to the guest region.
    // Future risk: if the CPU wrote COHER_STATUS_HOST in the region, this
    // copy would overwrite its value (today the CPU does not write it).
    const uint32_t value = const_cast<const volatile uint32_t&>(register_file_.values[reg]);
    ::gpu::WriteRegister(reg, value);
    return value;
  }
  return ::gpu::ReadRegister(reg);  // outside the register file: 0
}

}  // namespace gpu
}  // namespace xe
