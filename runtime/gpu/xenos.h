// Xenos formats shared by the GPU backends and the Vd* functions: PM4 opcodes, register
// indices, packet headers, physical address translation.
//
// Values from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
// src/xenia/gpu/xenos.h (Type3Opcode, MakePacketType*, kSwapSignature) and
// src/xenia/gpu/register_table.inc (register indices).
#pragma once

#include <cstdint>

namespace gpu {

// Type 3 (Xenia xenos.h Type3Opcode).
inline constexpr uint32_t kPm4Nop = 0x10;
inline constexpr uint32_t kPm4RegRmw = 0x21;
inline constexpr uint32_t kPm4DrawIndx = 0x22;
inline constexpr uint32_t kPm4SetConstant = 0x2D;
inline constexpr uint32_t kPm4IndirectBufferPfd = 0x37;
inline constexpr uint32_t kPm4WaitRegMem = 0x3C;
inline constexpr uint32_t kPm4MemWrite = 0x3D;
inline constexpr uint32_t kPm4RegToMem = 0x3E;
inline constexpr uint32_t kPm4IndirectBuffer = 0x3F;
inline constexpr uint32_t kPm4CondWrite = 0x45;
inline constexpr uint32_t kPm4EventWrite = 0x46;
inline constexpr uint32_t kPm4MeInit = 0x48;
inline constexpr uint32_t kPm4SetBinMask = 0x50;
inline constexpr uint32_t kPm4SetBinSelect = 0x51;
inline constexpr uint32_t kPm4Interrupt = 0x54;
inline constexpr uint32_t kPm4EventWriteShd = 0x58;
inline constexpr uint32_t kPm4EventWriteExt = 0x5A;
inline constexpr uint32_t kPm4EventWriteZpd = 0x5B;
inline constexpr uint32_t kPm4SetBinMaskLo = 0x60;
inline constexpr uint32_t kPm4SetBinMaskHi = 0x61;
inline constexpr uint32_t kPm4SetBinSelectLo = 0x62;
inline constexpr uint32_t kPm4SetBinSelectHi = 0x63;
inline constexpr uint32_t kPm4XeSwap = 0x64;  // Xenia only (and here): written by VdSwap

// Registers (DWORD indices, Xenia register_table.inc).
inline constexpr uint32_t kRegisterCount = 0x5003;  // Xenia RegisterFile::kRegisterCount
inline constexpr uint32_t kRegCpRbRptr = 0x01C4;
inline constexpr uint32_t kRegCpRbWptr = 0x01C5;
inline constexpr uint32_t kRegScratchUmsk = 0x01DC;
inline constexpr uint32_t kRegScratchAddr = 0x01DD;
inline constexpr uint32_t kRegScratchReg0 = 0x0578;  // .. SCRATCH_REG7 = 0x057F
inline constexpr uint32_t kRegCoherStatusHost = 0x0A31;
inline constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
inline constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
inline constexpr uint32_t kRegShaderConstantFetch00 = 0x4800;

inline constexpr uint32_t kSwapSignature = 0x53574150;  // 'SWAP' (Xenia kSwapSignature)

// Guest address of a GPU physical address (Xenia TranslatePhysical): the
// 0xA0000000 view, which shares memory with 0xC0000000 and 0xE0000000
// (GuestMemory); inverse of MmGetPhysicalAddress = addr & 0x1FFFFFFF.
constexpr uint32_t PhysicalToGuest(uint32_t phys) { return 0xA0000000u | (phys & 0x1FFFFFFFu); }

// Packet headers (Xenia xenos.h MakePacketType*).
constexpr uint32_t MakePacketType0(uint32_t index, uint32_t count, bool one_reg = false) {
  return (((count - 1) & 0x3FFF) << 16) | (one_reg ? 0x8000u : 0u) | (index & 0x7FFF);
}
constexpr uint32_t MakePacketType2() { return 2u << 30; }
constexpr uint32_t MakePacketType3(uint32_t opcode, uint32_t count, bool predicate = false) {
  return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8) | (predicate ? 1u : 0u);
}

}  // namespace gpu
