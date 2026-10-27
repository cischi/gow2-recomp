// Vd* exports (video), common to the GPU backends: all 20 imported by the game.
//
// Each function cites its Xenia counterpart
// (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc. Fixed video configuration:
// 1280x720, 60 Hz, progressive, widescreen, HD, NTSC (Xenia VdQueryVideoMode).
// The functions that on the console train or configure the video hardware
// (EDRAM, HSIO, clock gating, display modes) are safe stubs that
// report success like Xenia: there is no video hardware to configure.
#include "kernel/registry.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "gpu/gpu_system.h"
#include "gpu/xenos.h"
#include "imports.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "log.h"

namespace gpu {
namespace {

// Xenia xbox.h X_VIDEO_MODE (48 byte).
struct VideoMode {
  be<uint32_t> display_width;
  be<uint32_t> display_height;
  be<uint32_t> is_interlaced;
  be<uint32_t> is_widescreen;
  be<uint32_t> is_hi_def;
  be<float> refresh_rate;
  be<uint32_t> video_standard;
  be<uint32_t> unknown_0x8a;
  be<uint32_t> unknown_0x01;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(VideoMode) == 48);

void FillVideoMode(VideoMode* m) {
  std::memset(m, 0, sizeof(*m));
  m->display_width = 1280;
  m->display_height = 720;
  m->is_interlaced = 0;
  m->is_widescreen = 1;
  m->is_hi_def = 1;
  m->refresh_rate = 60.0f;
  m->video_standard = 1;  // NTSC
  m->unknown_0x8a = 0x4A;
  m->unknown_0x01 = 0x01;
}

// Xenia X_DISPLAY_INFO (0x58 bytes) and substructures.
struct D3dPrivateRect {
  be<uint32_t> x1, y1, x2, y2;
};
struct D3dFilterParameters {
  be<float> nyquist, flicker_filter, beta;
};
struct D3dPrivateScalerParameters {
  D3dPrivateRect scaler_source_rect;                // 0x00
  be<uint32_t> scaled_output_width;                 // 0x10
  be<uint32_t> scaled_output_height;                // 0x14
  be<uint32_t> vertical_filter_type;                // 0x18
  D3dFilterParameters vertical_filter_parameters;   // 0x1C
  be<uint32_t> horizontal_filter_type;              // 0x28
  D3dFilterParameters horizontal_filter_parameters; // 0x2C
};
static_assert(sizeof(D3dPrivateScalerParameters) == 0x38);
struct DisplayInfo {
  be<uint16_t> front_buffer_width;                // 0x00
  be<uint16_t> front_buffer_height;               // 0x02
  uint8_t front_buffer_color_format;              // 0x04
  uint8_t front_buffer_pixel_format;              // 0x05
  uint8_t pad_06[2];
  D3dPrivateScalerParameters scaler_parameters;   // 0x08
  be<uint16_t> display_window_overscan_left;      // 0x40
  be<uint16_t> display_window_overscan_top;       // 0x42
  be<uint16_t> display_window_overscan_right;     // 0x44
  be<uint16_t> display_window_overscan_bottom;    // 0x46
  be<uint16_t> display_width;                     // 0x48
  be<uint16_t> display_height;                    // 0x4A
  be<float> display_refresh_rate;                 // 0x4C
  be<uint32_t> display_interlaced;                // 0x50
  uint8_t display_color_format;                   // 0x54
  uint8_t pad_55;
  be<uint16_t> actual_display_width;              // 0x56
};
static_assert(sizeof(DisplayInfo) == 0x58);
static_assert(offsetof(DisplayInfo, display_window_overscan_left) == 0x40);
static_assert(offsetof(DisplayInfo, actual_display_width) == 0x56);

// ---- video configuration ---------------------------------------------------------

// Xenia: VdQueryVideoMode_entry.
void VdQueryVideoModeImpl(VideoMode* mode) {
  if (mode) FillVideoMode(mode);
}

// Xenia: VdQueryVideoFlags_entry. widescreen | width >= 1024.
uint32_t VdQueryVideoFlagsImpl() { return 1 | 2; }

// Xenia: VdGetCurrentDisplayInformation_entry.
void VdGetCurrentDisplayInformationImpl(DisplayInfo* info) {
  if (!info) return;
  VideoMode mode;
  FillVideoMode(&mode);
  const uint32_t w = mode.display_width, h = mode.display_height;
  std::memset(info, 0, sizeof(*info));
  info->front_buffer_width = uint16_t(w);
  info->front_buffer_height = uint16_t(h);
  info->scaler_parameters.scaler_source_rect.x2 = w;
  info->scaler_parameters.scaler_source_rect.y2 = h;
  info->scaler_parameters.scaled_output_width = w;
  info->scaler_parameters.scaled_output_height = h;
  info->scaler_parameters.horizontal_filter_type = 1;
  info->scaler_parameters.vertical_filter_type = 1;
  info->display_window_overscan_left = 320;
  info->display_window_overscan_top = 180;
  info->display_window_overscan_right = 320;
  info->display_window_overscan_bottom = 180;
  info->display_width = uint16_t(w);
  info->display_height = uint16_t(h);
  info->display_refresh_rate = float(mode.refresh_rate);
  info->actual_display_width = uint16_t(w);
}

// Xenia: VdGetCurrentDisplayGamma_entry, with Xenia's defaults: type 2
// (BT.709, HD TV) and power 2.22222233.
void VdGetCurrentDisplayGammaImpl(be<uint32_t>* type, be<float>* power) {
  if (type) *type = 2;
  if (power) *power = 2.22222233f;
}

// Xenia: VdSetDisplayMode_entry. Safe stub: no display to configure.
uint32_t VdSetDisplayModeImpl(uint32_t /*flags*/) { return 0; }

// ---- engines and hardware ---------------------------------------------------------

// Xenia: VdInitializeEngines_entry (PFP/ME microcode ignored). Success.
uint32_t VdInitializeEnginesImpl(uint32_t /*unk0*/, uint32_t /*callback*/, uint32_t /*arg*/, uint32_t /*pfp*/,
                                 uint32_t /*me*/) {
  return 1;
}

// Xenia: VdShutdownEngines_entry. Games call it between two
// initializations: the ring and the threads stay active.
void VdShutdownEnginesImpl() {}

// Xenia: VdIsHSIOTrainingSucceeded_entry. Safe stub: TRUE.
uint32_t VdIsHSIOTrainingSucceededImpl() { return 1; }

// Xenia: VdRetrainEDRAM_entry / VdRetrainEDRAMWorker_entry. Safe stubs:
// no EDRAM to retrain, success (0).
uint32_t VdRetrainEDRAMImpl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t VdRetrainEDRAMWorkerImpl(uint32_t) { return 0; }

// Xenia: VdEnableDisableClockGating_entry. Safe stub: ignored, 0.
uint32_t VdEnableDisableClockGatingImpl(uint32_t /*enabled*/) { return 0; }

// Xenia: VdPersistDisplay_entry. *unk1_ptr receives a physical block that the
// game frees with MmFreePhysicalMemory; Xenia allocates 64 bytes (here a
// page, the granularity of the physical heap). TRUE.
uint32_t VdPersistDisplayImpl(uint32_t /*unk0*/, be<uint32_t>* unk1_ptr) {
  if (unk1_ptr) {
    kernel::MemorySystem* m = kernel::Memory();
    *unk1_ptr = m ? m->phys.Alloc(64, 32) : 0;
  }
  return 1;
}

// Xenia: VdCallGraphicsNotificationRoutines_entry. No routine
// registered (the game does not import VdRegisterGraphicsNotificationRoutine): 0.
uint32_t VdCallGraphicsNotificationRoutinesImpl(uint32_t /*unk0*/, uint32_t /*args_ptr*/) { return 0; }

// ---- ring buffer, interrupt, swap ------------------------------------------------

// Xenia: VdSetGraphicsInterruptCallback_entry -> GraphicsSystem::SetInterruptCallback.
void VdSetGraphicsInterruptCallbackImpl(uint32_t callback, uint32_t user_data) {
  SetInterruptCallback(callback, user_data);
}

// Xenia: VdInitializeRingBuffer_entry. ptr = physical address (MmGetPhysicalAddress).
void VdInitializeRingBufferImpl(uint32_t ptr, uint32_t size_log2) { InitializeRingBuffer(ptr, size_log2); }

// Xenia: VdEnableRingBufferRPtrWriteBack_entry. ptr = physical address.
void VdEnableRingBufferRPtrWriteBackImpl(uint32_t ptr, uint32_t block_size_log2) {
  EnableReadPointerWriteBack(ptr, block_size_log2);
}

// Xenia: VdGetSystemCommandBuffer_entry: 0x94 zeroed bytes and the 0xBEEF000x markers.
void VdGetSystemCommandBufferImpl(be<uint32_t>* p0, be<uint32_t>* p1) {
  if (p0) {
    std::memset(p0, 0, 0x94);
    *p0 = 0xBEEF0000;
  }
  if (p1) *p1 = 0xBEEF0001;
}

// Xenia: VdSetSystemCommandBufferGpuIdentifierAddress_entry. Safe stub
// (Xenia ignores it too): the address is only used by the system buffer, which
// no backend executes.
void VdSetSystemCommandBufferGpuIdentifierAddressImpl(uint32_t /*addr*/) {}

// Xenia: VdInitializeScalerCommandBuffer_entry: dest filled with NOPs (type 2),
// returns the number of DWORDs. Deviation: a dest_count that exceeds the end
// of the guest space is rejected (0, nothing written).
uint32_t VdInitializeScalerCommandBufferImpl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                             uint32_t, uint32_t, uint32_t, be<uint32_t>* dest, uint32_t dest_count) {
  if (dest && !kernel::GuestRangeValid(kernel::GuestAddr(dest), uint64_t(dest_count) * 4)) {
    LogLine("[gpu] VdInitializeScalerCommandBuffer: dest " + std::to_string(kernel::GuestAddr(dest)) + " + " +
            std::to_string(dest_count) + " DWORDs beyond the guest space, rejected");
    return 0;
  }
  if (dest)
    for (uint32_t i = 0; i < dest_count; ++i) dest[i] = MakePacketType2();
  return dest_count;
}

// Xenia: VdSwap_entry. In the 64 DWORDs reserved by the game in the ring: type 0
// with the front buffer's fetch constant (physical address), PM4_XE_SWAP
// (signature, physical front buffer, width, height), then type 2 NOPs.
// The frame counts as presented (stats). Deviation: with null parameters
// (the console requires them all) the missing fields are 0 instead of an assert.
void VdSwapImpl(be<uint32_t>* buffer, be<uint32_t>* fetch, uint32_t /*unk2*/, uint32_t /*unk3*/, uint32_t /*unk4*/,
                be<uint32_t>* frontbuffer_ptr, be<uint32_t>* /*texture_format*/, be<uint32_t>* /*color_space*/,
                be<uint32_t>* width, be<uint32_t>* height) {
  kernel::stats::NoteFramePresented();
  if (!buffer) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      LogLine("[gpu] VdSwap without a buffer in the ring: frame counted, no packets");
    }
    return;
  }
  uint32_t f[6] = {};
  if (fetch)
    for (int i = 0; i < 6; ++i) f[i] = fetch[i];
  // D3D fetch constant: CPU address of the front buffer in dword_1 >> 12;
  // the GPU wants the physical one.
  const uint32_t fb_virtual = f[1] & 0xFFFFF000u;
  const uint32_t fb_physical = gow2::PhysicalAddressOf(fb_virtual);  // MmGetPhysicalAddress (0xE: +0x1000)
  (void)frontbuffer_ptr;
  f[1] = (f[1] & 0xFFFu) | fb_physical;

  std::memset(buffer, 0, 64 * 4);
  uint32_t o = 0;
  buffer[o++] = MakePacketType0(kRegShaderConstantFetch00, 6);
  for (uint32_t v : f) buffer[o++] = v;
  buffer[o++] = MakePacketType3(kPm4XeSwap, 4);
  buffer[o++] = kSwapSignature;
  buffer[o++] = fb_physical;
  buffer[o++] = width ? uint32_t(*width) : 0u;
  buffer[o++] = height ? uint32_t(*height) : 0u;
  while (o < 64) buffer[o++] = MakePacketType2();
}

}  // namespace

// Xenia: RegisterVideoExports (variables VdGlobalDevice, VdGlobalXamDevice,
// VdGpuClockInMHz, VdHSIOCalibrationLock).
void InitVideoVariables(std::span<const ImportRecord> records) {
  for (const ImportRecord& r : records) {
    if (r.type != 0) continue;
    const std::string name = ImportName(r);
    const uint32_t area = *kernel::GuestPtr<be<uint32_t>>(r.thunk);
    if (!area) continue;
    if (name == "VdGlobalDevice" || name == "VdGlobalXamDevice") {
      *kernel::GuestPtr<be<uint32_t>>(area) = 0;
    } else if (name == "VdGpuClockInMHz") {
      *kernel::GuestPtr<be<uint32_t>>(area) = 500;  // Xenos: 500 MHz
    } else if (name == "VdHSIOCalibrationLock") {
      kernel::CriticalSectionInit(area, 10000);
    } else {
      continue;
    }
    LogLine("[gpu] variable " + name + " initialized");
  }
}

}  // namespace gpu

using namespace gpu;
KERNEL_EXPORT(VdQueryVideoMode, VdQueryVideoModeImpl)
KERNEL_EXPORT(VdQueryVideoFlags, VdQueryVideoFlagsImpl)
KERNEL_EXPORT(VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformationImpl)
KERNEL_EXPORT(VdGetCurrentDisplayGamma, VdGetCurrentDisplayGammaImpl)
KERNEL_EXPORT(VdSetDisplayMode, VdSetDisplayModeImpl)
KERNEL_EXPORT(VdInitializeEngines, VdInitializeEnginesImpl)
KERNEL_EXPORT(VdShutdownEngines, VdShutdownEnginesImpl)
KERNEL_EXPORT(VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceededImpl)
KERNEL_EXPORT(VdRetrainEDRAM, VdRetrainEDRAMImpl)
KERNEL_EXPORT(VdRetrainEDRAMWorker, VdRetrainEDRAMWorkerImpl)
KERNEL_EXPORT(VdEnableDisableClockGating, VdEnableDisableClockGatingImpl)
KERNEL_EXPORT(VdPersistDisplay, VdPersistDisplayImpl)
KERNEL_EXPORT(VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutinesImpl)
KERNEL_EXPORT(VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallbackImpl)
KERNEL_EXPORT(VdInitializeRingBuffer, VdInitializeRingBufferImpl)
KERNEL_EXPORT(VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBackImpl)
KERNEL_EXPORT(VdGetSystemCommandBuffer, VdGetSystemCommandBufferImpl)
KERNEL_EXPORT(VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddressImpl)
KERNEL_EXPORT(VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBufferImpl)
KERNEL_EXPORT(VdSwap, VdSwapImpl)
