// Shim of Xenia's src/xenia/gpu/graphics_system.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause).
//
// In Xenia GraphicsSystem owns the CommandProcessor, handles the GPU
// registers as MMIO (the ring's write pointer comes from WriteRegister), the
// vblank thread and the interrupts. In gow2 these parts belong to the common
// GPU module (runtime/gpu/gpu_system.h: big-endian DWORD registers in the guest
// region 0x7FC80000, vblank, DispatchInterrupt). This shim has no MMIO or
// thread of its own: it holds Xenia's RegisterFile (host-endian, the command
// processor's), owns the CommandProcessor and bridges to the guest region for
// the gow2 patches to the vendored code:
//  - gow2_PollWritePointer: CP_RB_WPTR read from the guest region (the CPU
//    writes it to memory, there is no notification), 0xBAADF00D until the ring
//    is initialized;
//  - gow2_MirrorRegister: every command processor write to a register
//    (final value) and the read pointer (CP_RB_RPTR) also go to the guest
//    region, where the CPU reads them;
//  - gow2_ReadRegisterForWait: WAIT_REG_MEM on a register reads the guest
//    region (values written by the CPU), except COHER_STATUS_HOST, which
//    MakeCoherent clears in the RegisterFile: that value is read from the
//    RegisterFile and also copied to the guest region, where the CPU reads it.
#ifndef XENIA_GPU_GRAPHICS_SYSTEM_H_
#define XENIA_GPU_GRAPHICS_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "xenia/gpu/register_file.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/presenter.h"

namespace xe {
namespace gpu {

class CommandProcessor;

class GraphicsSystem {
 public:
  // provider and presenter may be nullptr (no GPU); not owned.
  GraphicsSystem(Memory* memory, ui::GraphicsProvider* provider, ui::Presenter* presenter);
  virtual ~GraphicsSystem();
  GraphicsSystem(const GraphicsSystem&) = delete;
  GraphicsSystem& operator=(const GraphicsSystem&) = delete;

  Memory* memory() const { return memory_; }
  kernel::KernelState* kernel_state() { return &kernel_state_; }
  ui::GraphicsProvider* provider() const { return provider_; }
  ui::Presenter* presenter() const { return presenter_; }
  RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }
  // xenia-canary: output aspect ratio passed to the presenter
  // (16:9 from GraphicsSystem::Setup; gow2's presenter ignores it).
  std::pair<uint32_t, uint32_t> GetScaledAspectRatio() const { return {16, 9}; }

  // Xenia: CreateCommandProcessor in Setup. Here the backend (gpu::
  // CreateXeniaBackend) creates it with its factory and hands it over; the
  // CommandProcessor dies before the RegisterFile.
  void gow2_SetCommandProcessor(std::unique_ptr<CommandProcessor> command_processor);

  // Xenia: forwarded to the CommandProcessor (VdInitializeRingBuffer,
  // VdEnableRingBufferRPtrWriteBack). After InitializeRingBuffer the write
  // pointer in the guest region is valid for the command processor.
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  // Xenia: callback(source, user_data) on the current thread with CPU `cpu`
  // active -> gpu::DispatchInterrupt (FIFO, PCR TLS at 0).
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  // Xenia: host device loss from any thread (in Xenia the UI recreates the
  // device). Here: line `[gpu] device removed reason=0x%08X`
  // (gow2_DeviceRemovedReason), then xe::FatalError: summary (main's hook)
  // and exit 5. Does not return. Must be called without the global lock (it is:
  // D3D12CommandProcessor::BeginSubmission).
  void OnHostGpuLossFromAnyThread(bool is_responsible);
  // HRESULT of the host device's GetDeviceRemovedReason; 0 without a device
  // (D3D12GraphicsSystem reads it from the provider).
  virtual uint32_t gow2_DeviceRemovedReason() const { return 0; }

  // Register bridge (gow2 patch to command_processor.cc), see above.
  uint32_t gow2_PollWritePointer() const;
  void gow2_MirrorRegister(uint32_t reg, uint32_t value);
  uint32_t gow2_ReadRegisterForWait(uint32_t reg) const;

 private:
  Memory* memory_;
  ui::GraphicsProvider* provider_;
  ui::Presenter* presenter_;
  kernel::KernelState kernel_state_;
  std::atomic<bool> ring_initialized_{false};
  std::atomic_flag host_gpu_loss_reported_ = ATOMIC_FLAG_INIT;
  RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GRAPHICS_SYSTEM_H_
