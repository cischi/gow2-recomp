// XAudio* render-driver exports (xboxkrnl), on top of the audio system in
// audio_system.h. Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc.
//
// tools/gen_import_stubs.py also reads KERNEL_EXPORT from the .cpp files in
// this folder (cmake/Recompile.cmake, --implemented-from runtime/apu).
#include "kernel/registry.h"

#include <cstdint>

#include "apu/audio_system.h"
#include "kernel/guest.h"

namespace apu {
namespace {

// Xenia: XAudioRegisterRenderDriverClient_entry (callback_ptr[0] = callback,
// callback_ptr[1] = argument; handle 0x41550000 | index in *driver_ptr).
uint32_t XAudioRegisterRenderDriverClientImpl(uint32_t callback_ptr, uint32_t driver_ptr) {
  return RegisterClient(callback_ptr, driver_ptr);
}
// Xenia: XAudioUnregisterRenderDriverClient_entry (the handle is the "pointer").
uint32_t XAudioUnregisterRenderDriverClientImpl(uint32_t driver) { return UnregisterClient(driver); }
// Xenia: XAudioSubmitRenderDriverFrame_entry.
uint32_t XAudioSubmitRenderDriverFrameImpl(uint32_t driver, uint32_t samples) {
  return SubmitFrame(driver, samples);
}
// Xenia: XAudioGetVoiceCategoryVolume_entry (1.0f) e
// XAudioGetVoiceCategoryVolumeChangeMask_entry (0).
uint32_t XAudioGetVoiceCategoryVolumeImpl(uint32_t /*category*/, uint32_t out) {
  if (be<uint32_t>* p = kernel::GuestPtr<be<uint32_t>>(out)) *p = 0x3F800000;
  return 0;
}
uint32_t XAudioGetVoiceCategoryVolumeChangeMaskImpl(uint32_t /*driver*/, uint32_t out) {
  if (be<uint32_t>* p = kernel::GuestPtr<be<uint32_t>>(out)) *p = 0;
  return 0;
}

}  // namespace
}  // namespace apu

KERNEL_EXPORT(XAudioRegisterRenderDriverClient, apu::XAudioRegisterRenderDriverClientImpl)
KERNEL_EXPORT(XAudioUnregisterRenderDriverClient, apu::XAudioUnregisterRenderDriverClientImpl)
KERNEL_EXPORT(XAudioSubmitRenderDriverFrame, apu::XAudioSubmitRenderDriverFrameImpl)
KERNEL_EXPORT(XAudioGetVoiceCategoryVolume, apu::XAudioGetVoiceCategoryVolumeImpl)
KERNEL_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask, apu::XAudioGetVoiceCategoryVolumeChangeMaskImpl)
