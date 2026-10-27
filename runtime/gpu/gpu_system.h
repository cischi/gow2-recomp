// GPU module common to the backends: register region,
// vblank, interrupts to the game, ring configuration forwarded to the
// backend, Vd* variables. The backend (gpu_fake::CreateBackend, then D3D12)
// owns the command processor thread.
//
// Model from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/gpu/graphics_system.cc (registri MMIO, thread "GPU VSync",
// SetInterruptCallback, DispatchInterruptCallback, MarkVblank).
//
//  - Registers: the region 0x7FC80000-0x7FCFFFFF is committed and holds the
//    register file (register i = big-endian DWORD at 0x7FC80000 + i*4). The
//    game's reads and writes are plain memory accesses (Xenia
//    uses an MMIO handler instead); the registers that Xenia reads with a
//    fixed value (RB_EDRAM_TIMING, RB_BC_CONTROL, D1MODE_V_COUNTER, interrupt
//    status, D1MODE_VIEWPORT_SIZE) are rewritten on every vblank.
//  - Vblank: a guest thread with a host body, every 16.667 ms, calls
//    Backend::OnVblank (CP counter + 1) and callback(0, user_data) with the
//    active CPU 2 (Xenia MarkVblank -> DispatchInterruptCallback(0, 2)).
//  - Interrupts: one at a time, served in arrival order (ticket lock).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

struct GuestMemory;
struct ImportRecord;

namespace gpu {

inline constexpr uint32_t kRegisterBase = 0x7FC80000;
inline constexpr uint32_t kRegisterRegionSize = 0x80000;  // up to 0x7FCFFFFF
inline constexpr std::chrono::microseconds kVblankPeriod{16667};
// Period of the emulated vblank (default kVblankPeriod); the vblank rereads it on every iteration.
void SetVblankPeriod(std::chrono::microseconds);

// Backend chosen with --gpu.
enum class BackendKind { kFake, kD3D12 };
// Only "fake" and "d3d12" (lowercase); otherwise nullopt.
std::optional<BackendKind> ParseBackendKind(std::string_view);

// Command processor counters.
struct Counters {
  uint64_t packets = 0;          // packets read (all types, IB included)
  uint64_t unknown_packets = 0;  // type 3 not executed
  uint64_t frames = 0;           // XE_SWAP executed
};

class Backend {
 public:
  virtual ~Backend() = default;
  // Starts the command processor thread (GuestThread::CreateHost); false on failure.
  virtual bool Start() = 0;
  // Stops the thread and waits for it to finish; idempotent. A WAIT_REG_MEM in
  // progress is interrupted within 50 ms.
  virtual void Stop() = 0;
  // VdInitializeRingBuffer: ring at physical address `phys`,
  // 1 << (size_log2 + 3) bytes; the read pointer restarts from 0.
  virtual void InitializeRingBuffer(uint32_t phys, uint32_t size_log2) = 0;
  // VdEnableRingBufferRPtrWriteBack: physical address of the write-back (0 = none).
  virtual void EnableReadPointerWriteBack(uint32_t phys, uint32_t block_size_log2) = 0;
  // From the vblank thread: CP counter + 1 (Xenia MarkVblank).
  virtual void OnVblank() = 0;
  // Consistent copy at the time of the call.
  virtual Counters GetCounters() const = 0;
};

// Commits the register region, sets the fixed values, starts the backend
// and the vblank thread. Must be called after kernel::InitMemory and
// kernel::InitThreads. false if the commit, Start or thread creation
// fail. A second call without Shutdown returns true with no effect
// (the passed backend is discarded).
bool Init(GuestMemory&, std::unique_ptr<Backend>);
// Stops the vblank and the backend and waits for their threads. Idempotent; after
// Shutdown, Init can be called again.
void Shutdown();

// Register file (DWORD index; outside kRegisterCount: write
// ignored, read 0). Atomic accesses to the big-endian guest region.
uint32_t ReadRegister(uint32_t reg);
void WriteRegister(uint32_t reg, uint32_t value);

// Forwarded to the active backend (ignored without a backend).
void InitializeRingBuffer(uint32_t phys, uint32_t size_log2);
void EnableReadPointerWriteBack(uint32_t phys, uint32_t block_size_log2);

// VdSetGraphicsInterruptCallback (callback 0 = none).
void SetInterruptCallback(uint32_t callback, uint32_t user_data);
// Xenia GraphicsSystem::DispatchInterruptCallback: callback(source,
// user_data) on the current guest thread with active CPU `cpu`. One interrupt
// at a time, in arrival order (FIFO ticket lock), with the PCR's TLS pointer
// set to 0 during the callback. Does nothing without a callback.
void DispatchInterrupt(uint32_t source, uint32_t cpu);

// Vd* variables imported by the game (Xenia RegisterVideoExports,
// xboxkrnl_video.cc): VdGlobalDevice and VdGlobalXamDevice = 0,
// VdGpuClockInMHz = 500, VdHSIOCalibrationLock = initialized
// critical section (spin count 10000). The areas are the ones ResolveImports
// linked to the slots (type 0 records): call after ResolveImports.
void InitVideoVariables(std::span<const ImportRecord> records);

// Writes `[gpu] device removed reason=0x%08X` only once per process
// (the presenter and the command processor may both notice the loss).
// true if this call wrote the line.
bool LogDeviceRemovedOnce(uint32_t reason);

// Counters of the active backend (after Shutdown: the last ones read).
Counters GetCounters();

}  // namespace gpu
