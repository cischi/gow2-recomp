// Shim of xenia-canary's src/xenia/emulator.h
// (https://github.com/xenia-canary/xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause): D3D12CommandProcessor::GetWindowTitleText asks the Emulator
// for the audio system. Audio belongs to gow2 (runtime/apu), not to a Xenia
// AudioSystem: always nullptr. KernelState::emulator() (xenia/kernel/kernel_state.h)
// provides the instance.
#ifndef XENIA_EMULATOR_H_
#define XENIA_EMULATOR_H_

#include "xenia/apu/audio_system.h"

namespace xe {

class Emulator {
 public:
  apu::AudioSystem* audio_system() { return nullptr; }
};

}  // namespace xe

#endif  // XENIA_EMULATOR_H_
