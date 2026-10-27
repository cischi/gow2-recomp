// Shim of xenia-canary's src/xenia/apu/audio_system.h
// (https://github.com/xenia-canary/xenia-canary @ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad,
// BSD-3-Clause): D3D12CommandProcessor::GetWindowTitleText appends the
// Emulator's audio system name to the title. Audio belongs to gow2 (runtime/apu):
// Emulator::audio_system() is always nullptr (xenia/kernel/kernel_state.h).
#ifndef XENIA_APU_AUDIO_SYSTEM_H_
#define XENIA_APU_AUDIO_SYSTEM_H_

namespace xe {
namespace apu {

class AudioSystem {
 public:
  const char* name() const { return ""; }
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_SYSTEM_H_
