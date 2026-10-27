// Mini PM4 interpreter of the fake GPU.
//
// It executes only packets with effects observable by the CPU: register and
// memory writes, fences and events with a memory write, waits on a register
// or memory, interrupts, indirect buffers and VdSwap's swap. Draws, states
// and shader loading are skipped based on their length, counted and logged
// once per opcode (sub-project 3 will replace them with a renderer).
//
// Formats and semantics from Xenia (https://github.com/xenia-project/xenia @
// 95a5c3ee, BSD): src/xenia/gpu/xenos.h (Type3Opcode, GpuSwap, CpuToGpu,
// MakePacketType*) and src/xenia/gpu/command_processor.cc (ExecutePacket*,
// MakeCoherent). Addresses in packets: physical; they are read through the
// 0xA0000000 view (same memory as the 0xC/0xE views, see GuestMemory).
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "gpu/gpu_system.h"
#include "gpu/xenos.h"

namespace gpu_fake {

// Opcodes, registers, packet headers and Counters live in the common module
// (gpu/xenos.h, gpu/gpu_system.h): also visible as gpu_fake::.
using namespace gpu;

// EVENT_WRITE_ZPD: fake samples per tile (Xenia query_occlusion_fake_sample_count).
inline constexpr uint32_t kFakeSampleCount = 1000;

// Command processor state that survives between two ExecuteRing calls.
struct Pm4State {
  Counters counters;
  std::array<uint64_t, 128> unknown_by_opcode{};
  uint64_t bin_mask = ~0ull;    // Xenia bin_mask_/bin_select_: predicated packets
  uint64_t bin_select = ~0ull;
};

// Effects of packets outside guest memory. Empty std::functions mean
// "no effect" (an empty read_reg reads 0).
struct Pm4Effects {
  // Register write from a packet (with the command processor's side
  // effects, e.g. SCRATCH_REG and COHER_STATUS_HOST).
  std::function<void(uint32_t reg, uint32_t value)> write_reg;
  // Direct write to the register file, without side effects (Xenia
  // MakeCoherent clears COHER_STATUS_HOST this way); empty = write_reg.
  std::function<void(uint32_t reg, uint32_t value)> store_reg;
  std::function<uint32_t(uint32_t reg)> read_reg;
  // PM4_INTERRUPT: called once for each CPU in the packet's mask
  // (Xenia DispatchInterruptCallback(1, cpu)).
  std::function<void(uint32_t cpu)> interrupt;
  // XE_SWAP (physical address of the front buffer, width, height).
  std::function<void(uint32_t frontbuffer, uint32_t width, uint32_t height)> swap;
  // Vblank/swap counter (Xenia CommandProcessor::counter_), written by
  // EVENT_WRITE_SHD with bit 31 of the initiator; empty = 0.
  std::function<uint32_t()> counter;
  // An unsatisfied WAIT_REG_MEM waits while running() is true (empty =
  // always): on shutdown the GPU interrupts the wait.
  std::function<bool()> running;
  // Persistent state; nullptr = temporary state for this call.
  Pm4State* state = nullptr;
};

// Executes the packets in [read, write) of the ring (ring_size_dwords DWORDs
// starting at guest address ring_guest, with wrap-around); returns the
// new read pointer (in DWORDs), i.e. write. A truncated packet (longer than
// the remaining DWORDs) stops execution, like Xenia.
uint32_t ExecuteRing(uint8_t* base, uint32_t ring_guest, uint32_t ring_size_dwords, uint32_t read, uint32_t write,
                     const Pm4Effects&);

}  // namespace gpu_fake
