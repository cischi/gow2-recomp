// Shim of Xenia's src/xenia/kernel/kernel_state.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec and
// xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad, BSD-3-Clause): the
// CommandProcessor uses the kernel state for the executable module's title ID
// (trace file names, never opened here: TraceWriter is empty) and, in canary,
// for memory (InitializeRingBuffer), the idle process of the command
// processor thread, the guest stack for the log of
// log_ringbuffer_kickoff_initiator_bts (off: no stack here) and the audio
// system name in the window title (none). The real kernel is
// gow2's (runtime/kernel).
#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <cstdint>

#include "xenia/emulator.h"
#include "xenia/kernel/user_module.h"

namespace xe {

class Memory;

namespace cpu {
namespace backend {

// Like canary's xenia/cpu/backend/backend.h.
static constexpr uint32_t MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES = 32;
struct GuestPseudoStackTrace {
  uint32_t count;
  uint32_t truncated_flag;
  uint32_t return_addrs[MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES];
};

class Backend {
 public:
  // gow2 does not reconstruct guest stacks.
  bool PopulatePseudoStacktrace(GuestPseudoStackTrace* /*st*/) { return false; }
};

}  // namespace backend

class Processor {
 public:
  backend::Backend* backend() { return &backend_; }

 private:
  backend::Backend backend_;
};

}  // namespace cpu

namespace kernel {

class KernelState {
 public:
  explicit KernelState(Memory* memory = nullptr) : memory_(memory) {}

  // Never nullptr (Xenia dereferences it unchecked when tracing a frame).
  UserModule* GetExecutableModule() { return &executable_module_; }
  Memory* memory() const { return memory_; }
  // Guest process of the host threads (XHostThread ignores it).
  uint32_t GetIdleProcess() const { return 0; }
  cpu::Processor* processor() { return &processor_; }
  Emulator* emulator() { return &emulator_; }

 private:
  Memory* memory_;
  UserModule executable_module_;
  cpu::Processor processor_;
  Emulator emulator_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
