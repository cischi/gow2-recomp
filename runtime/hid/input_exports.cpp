// XamInput* exports on top of the input system in input_system.h. Each function
// cites its Xenia counterpart (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xam/xam_input.cc.
//
// tools/gen_import_stubs.py also reads KERNEL_EXPORT from the .cpp files in
// this folder (cmake/Recompile.cmake, --implemented-from runtime/hid).
#include "kernel/registry.h"

#include <cstdint>

#include "hid/input_system.h"

namespace hid {
namespace {

// Xenia: XamInputGetState_entry.
uint32_t XamInputGetStateImpl(uint32_t user, uint32_t flags, uint32_t state) { return GetState(user, flags, state); }
// Xenia: XamInputGetCapabilities_entry.
uint32_t XamInputGetCapabilitiesImpl(uint32_t user, uint32_t flags, uint32_t caps) {
  return GetCapabilities(user, flags, caps);
}
// Xenia: XamInputSetState_entry.
uint32_t XamInputSetStateImpl(uint32_t user, uint32_t unk, uint32_t vibration) {
  return SetState(user, unk, vibration);
}

}  // namespace
}  // namespace hid

KERNEL_EXPORT(XamInputGetState, hid::XamInputGetStateImpl)
KERNEL_EXPORT(XamInputGetCapabilities, hid::XamInputGetCapabilitiesImpl)
KERNEL_EXPORT(XamInputSetState, hid::XamInputSetStateImpl)
