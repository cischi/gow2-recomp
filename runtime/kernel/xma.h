// Registers and contexts of the XMA decoder: register block 0x7FEA0000-0x7FEAFFFF
// and the array of contexts, as the game sees them. The decoding (Xenia's XmaContext,
// "XMA Decoder" worker) is in runtime/apu/xma_decoder, hooked up with
// SetRegisterListener and SetContextListener.
//
// Model from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/apu/xma_decoder.cc (Setup, ReadRegister, WriteRegister,
// AllocateContext/ReleaseContext), xma_register_table.inc,
// src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc (XMACreateContext,
// XMAReleaseContext).
//
//  - Registers: Xenia serves them with an MMIO handler; here the region is
//    committed as normal memory. The APU registers are little-endian
//    (the game reads them with lwbrx: Xenia, cpu/mmio_handler.cc, swaps the bytes
//    for loads without movbe), so register r lives at 0x7FEA0000 + r*4
//    as a little-endian DWORD. Resting values like XmaRegisterFile: all 0,
//    except ContextArrayAddress (0x600) = physical address of the context
//    array and NextContextIndex (0x607) = 1.
//  - Contexts: 320 XMA_CONTEXT_DATA structures of 64 bytes in a single block
//    of the physical heap (aligned to 256, like Xenia's SystemHeapAlloc);
//    XMACreateContext hands out a free one, XMAReleaseContext zeroes it and
//    makes it available again.
#pragma once

#include <cstdint>

struct GuestMemory;

namespace kernel::xma {

inline constexpr uint32_t kRegisterBase = 0x7FEA0000;
inline constexpr uint32_t kRegisterRegionSize = 0x10000;
inline constexpr uint32_t kContextCount = 320;
inline constexpr uint32_t kContextSize = 64;

// Register indices (xma_register_table.inc).
inline constexpr uint32_t kRegContextArrayAddress = 0x600;
inline constexpr uint32_t kRegCurrentContextIndex = 0x606;
inline constexpr uint32_t kRegNextContextIndex = 0x607;
inline constexpr uint32_t kRegContext0Kick = 0x650;
inline constexpr uint32_t kRegContext0Lock = 0x690;
inline constexpr uint32_t kRegContext0Clear = 0x6A0;

// Commits the registers, allocates the context array in the physical heap and writes
// the resting values. Must be called after kernel::InitMemory. false if the commit
// or the allocation fails. A second call without Shutdown returns
// true with no effect.
bool Init(GuestMemory&);
// Frees the context array (if the physical heap is still there). Idempotent.
void Shutdown();

// Register file (index in DWORDs; outside the region: read 0, write
// ignored). Natural values, stored little-endian. Atomic accesses
// (std::atomic_ref): the game, the decoder and the hook touch them from different
// threads.
uint32_t ReadRegister(uint32_t reg);
void WriteRegister(uint32_t reg, uint32_t value);

// Guest address (virtual, in the physical heap) of the first context; 0 before Init.
uint32_t ContextArray();
// Free context (guest address, zeroed) or 0 if all are in use or
// before Init.
uint32_t AllocateContext();
// Makes the context available again (zeroing it); false if `ctx` is not
// an allocated context.
bool ReleaseContext(uint32_t ctx);

// Allocation and release of contexts (XMA decoder, runtime/apu/xma_decoder):
// called under this module's mutex after AllocateContext has
// zeroed the context (allocated = true) and before ReleaseContext zeroes it
// (allocated = false). The listener must not call back into functions of this
// module that take the mutex (Allocate/Release/ContextArray).
using ContextListener = void (*)(uint32_t index, bool allocated);
// nullptr (default): no notification.
void SetContextListener(ContextListener);

// Game writes to the registers (sub-project 4). The recompiled code
// emits PPC_MM_STORE_U32 for stores followed by eieio; runtime/ppc_detail.h
// redefines it so that, after the normal store, it calls gow2_mmio_store32
// for addresses in the register region.
//
// Kick (0x650-0x659), Lock (0x690-0x699) and Clear (0x6A0-0x6A9): bit b of
// register base+n is context 32n + b.
enum class RegisterOp { kKick, kLock, kClear };
// Affected contexts: first_context + b for every bit b set to 1 in `bits`.
// Called on the game thread that writes the register.
using RegisterListener = void (*)(RegisterOp, uint32_t first_context, uint32_t bits);
// nullptr (default): inert contexts.
void SetRegisterListener(RegisterListener);
// Game write (natural value of the register). Updates the register
// file; Kick/Lock/Clear with at least one bit set to 1 -> listener; Kick ->
// stats::NoteXmaKicks(popcount(bits)). Outside the region: ignored.
void OnRegisterWrite(uint32_t reg, uint32_t value);

}  // namespace kernel::xma

// Hook of PPC_MM_STORE_U32 (runtime/ppc_detail.h): `value` is the little-endian
// DWORD written to memory, i.e. the register's natural value.
// -> OnRegisterWrite((guest_address - kRegisterBase) / 4, value). Addresses
// not 4-aligned: ignored (the normal store has already happened). With --trace
// the first 32 calls write `[xma] reg 0x%03X = 0x%08X` to the log.
extern "C" void gow2_mmio_store32(uint32_t guest_address, uint32_t value);
