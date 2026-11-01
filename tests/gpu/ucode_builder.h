// Minimal Xenos microcode for the GPU tests, built field by field
// without an assembler. The bit layouts are those of the structs in
// thirdparty/xenia/src/xenia/gpu/ucode.h (Xenia @ 95a5c3ee, BSD-3-Clause):
// ControlFlowExecInstruction, ControlFlowAllocInstruction,
// VertexFetchInstruction, TextureFetchInstruction, AluInstruction. The
// private fields of those structs are not writable, so here the
// dwords are packed and the tests read them back with the structs' accessors.
//
// Structure of a program (ucode.h, Shader::AnalyzeUcode): first the
// control instructions, two every 3 dwords; then the 3-dword ALU/fetch
// instructions. The first exec has as its address the number of control pairs.
// The dwords are in host order (Shader(..., std::endian::native)).
#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

namespace gow2::ucode_builder {

using xe::gpu::ucode::AllocType;
using xe::gpu::ucode::AluScalarOpcode;
using xe::gpu::ucode::AluVectorOpcode;
using xe::gpu::ucode::ControlFlowOpcode;
using xe::gpu::ucode::ExportRegister;
using xe::gpu::ucode::FetchOpcode;

// 48-bit control instruction: complete dword_0, low 16 bits of dword_1.
struct Cf {
  uint32_t dword_0 = 0;
  uint32_t dword_1 = 0;
};

inline Cf CfExec(bool end, uint32_t address, uint32_t count, uint32_t sequence) {
  Cf cf;
  cf.dword_0 = (address & 0xFFF) | ((count & 0x7) << 12) | ((sequence & 0xFFF) << 16);
  // is_predicate_clean = 1 (bit 9), opcode in bits 12-15.
  ControlFlowOpcode op = end ? ControlFlowOpcode::kExecEnd : ControlFlowOpcode::kExec;
  cf.dword_1 = (1u << 9) | (uint32_t(op) << 12);
  return cf;
}

inline Cf CfAlloc(AllocType type, uint32_t size) {
  Cf cf;
  cf.dword_0 = size & 0x7;
  cf.dword_1 = (uint32_t(type) << 9) | (uint32_t(ControlFlowOpcode::kAlloc) << 12);
  return cf;
}

inline Cf CfNop() { return Cf{}; }

// Sequence of an exec: for each instruction 2 bits, [0] fetch, [1] serialize.
inline uint32_t Seq(std::initializer_list<uint32_t> per_instr) {
  uint32_t seq = 0, shift = 0;
  for (uint32_t bits : per_instr) {
    seq |= (bits & 0b11) << shift;
    shift += 2;
  }
  return seq;
}
constexpr uint32_t kSeqAlu = 0b00;
constexpr uint32_t kSeqFetch = 0b01;
constexpr uint32_t kSeqAluSerialize = 0b10;

struct Instr {
  uint32_t dwords[3] = {};
};

// Destination swizzle of a fetch: 3 bits per component
// (FetchDestinationSwizzle: 0-3 = x-w, 4 = 0, 5 = 1, 7 = keep).
constexpr uint32_t FetchDstSwizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
  return x | (y << 3) | (z << 6) | (w << 9);
}
constexpr uint32_t kFetchSwz0 = 4;
constexpr uint32_t kFetchSwz1 = 5;

// vfetch_full rDst.<dst_swizzle>, rSrc.x, vf<fetch_constant>.
inline Instr VertexFetch(uint32_t dst_reg, uint32_t dst_swizzle, uint32_t src_reg,
                         uint32_t fetch_constant, xe::gpu::xenos::VertexFormat format,
                         uint32_t stride_dwords, uint32_t offset_dwords) {
  Instr in;
  // Word 0: opcode, src_reg, dst_reg, must_be_one (bit 19), const_index
  // (fetch_constant / 3) and const_index_sel (fetch_constant % 3); src_swiz = x.
  in.dwords[0] = uint32_t(FetchOpcode::kVertexFetch) | ((src_reg & 0x3F) << 5) |
                 ((dst_reg & 0x3F) << 12) | (1u << 19) |
                 (((fetch_constant / 3) & 0x1F) << 20) | (((fetch_constant % 3) & 0x3) << 25);
  // Word 1: dst_swiz, fomat_comp_all = 1 (signed), num_format_all = 0
  // (normalized; irrelevant for floats), format.
  in.dwords[1] = (dst_swizzle & 0xFFF) | (1u << 12) | ((uint32_t(format) & 0x3F) << 16);
  // Word 2: stride and offset in dwords.
  in.dwords[2] = (stride_dwords & 0xFF) | ((offset_dwords & 0x7FFFFF) << 8);
  return in;
}

// tfetch2D rDst.xyzw, rSrc.xy, tf<fetch_constant>, OffsetX/OffsetY in texels
// (-8 to 7), filters from the constant.
inline Instr TextureFetch2D(uint32_t dst_reg, uint32_t src_reg, uint32_t fetch_constant, int32_t offset_x = 0,
                            int32_t offset_y = 0, bool register_gradients = false) {
  using xe::gpu::xenos::AnisoFilter;
  using xe::gpu::xenos::FetchOpDimension;
  using xe::gpu::xenos::TextureFilter;
  Instr in;
  // Word 0: opcode, src_reg, dst_reg, fetch_valid_only (bit 19), const_index,
  // absolute 3-component src_swiz of 2 bits each (x, y, z).
  in.dwords[0] = uint32_t(FetchOpcode::kTextureFetch) | ((src_reg & 0x3F) << 5) |
                 ((dst_reg & 0x3F) << 12) | (1u << 19) | ((fetch_constant & 0x1F) << 20) |
                 ((0u | (1u << 2) | (2u << 4)) << 26);
  // Word 1: dst_swiz xyzw, mag/min/mip/vol filter = kUseFetchConst, aniso =
  // kUseFetchConst, use_comp_lod = 1 (bit 28).
  const uint32_t fc = uint32_t(TextureFilter::kUseFetchConst);
  in.dwords[1] = FetchDstSwizzle(0, 1, 2, 3) | (fc << 12) | (fc << 14) | (fc << 16) |
                 (uint32_t(AnisoFilter::kUseFetchConst) << 18) | (fc << 24) | (fc << 26) |
                 (1u << 28);
  // Word 2: dimension (bits 14-15) = 2D, offset_x/offset_y (bits 16-20, 21-25,
  // signed, in half texels).
  in.dwords[2] = uint32_t(FetchOpDimension::k2D) << 14 | (uint32_t(offset_x * 2) & 0x1F) << 16 |
                 (uint32_t(offset_y * 2) & 0x1F) << 21 | uint32_t(register_gradients);
  return in;
}

// getWeights2D rDst, rSrc.xy, tf<fetch_constant>: the same encoding as tfetch2D with the opcode changed.
inline Instr TextureWeights2D(uint32_t dst_reg, uint32_t src_reg, uint32_t fetch_constant) {
  Instr in = TextureFetch2D(dst_reg, src_reg, fetch_constant);
  in.dwords[0] = (in.dwords[0] & ~0x1Fu) | uint32_t(FetchOpcode::kGetTextureWeights);
  return in;
}

// ALU source: temporary register or float constant, identity swizzle.
struct AluSrc {
  bool temp;
  uint32_t reg;
};
inline AluSrc R(uint32_t reg) { return {true, reg}; }
inline AluSrc C(uint32_t reg) { return {false, reg}; }

// Export of `src` (mov = max(src, src)) into the export register `dest`, xyzw.
inline Instr AluExportMov(ExportRegister dest, AluSrc src) {
  Instr in;
  // Word 0: vector_dest, export_data (bit 15), vector_write_mask = xyzw,
  // scalar_write_mask = 0, scalar_opc = kRetainPrev.
  in.dwords[0] = (uint32_t(dest) & 0x3F) | (1u << 15) | (0xFu << 16) |
                 (uint32_t(AluScalarOpcode::kRetainPrev) << 26);
  // Word 1: relative swizzles = 0 (identity), no negation/predicate.
  in.dwords[1] = 0;
  // Word 2: src1_reg, src2_reg, vector_opc = kMax, src1_sel/src2_sel (1 = temp).
  in.dwords[2] = ((src.reg & 0xFF) << 8) | ((src.reg & 0xFF) << 16) |
                 (uint32_t(AluVectorOpcode::kMax) << 24) | (uint32_t(src.temp) << 30) |
                 (uint32_t(src.temp) << 31);
  return in;
}

// rDst.xyzw = op(a, b), two-source vector operation (e.g. kMax, kAdd).
inline Instr AluVector(AluVectorOpcode op, uint32_t dst_reg, AluSrc a, AluSrc b) {
  Instr in;
  // Word 0: temporary vector_dest, vector_write_mask = xyzw, scalar_opc = kRetainPrev.
  in.dwords[0] = (dst_reg & 0x3F) | (0xFu << 16) | (uint32_t(AluScalarOpcode::kRetainPrev) << 26);
  in.dwords[1] = 0;
  // Word 2: src2_reg (bit 8), src1_reg (bit 16), vector_opc, src2_sel (bit 30), src1_sel (bit 31).
  in.dwords[2] = ((b.reg & 0xFF) << 8) | ((a.reg & 0xFF) << 16) | (uint32_t(op) << 24) | (uint32_t(b.temp) << 30) |
                 (uint32_t(a.temp) << 31);
  return in;
}

// Packs the control instructions in pairs (ucode.h,
// UnpackControlFlowInstructions in reverse) and appends the instructions.
inline std::vector<uint32_t> Assemble(std::vector<Cf> cfs, const std::vector<Instr>& instrs) {
  if (cfs.size() & 1) {
    cfs.push_back(CfNop());
  }
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cfs.size(); i += 2) {
    const Cf& a = cfs[i];
    const Cf& b = cfs[i + 1];
    out.push_back(a.dword_0);
    out.push_back((a.dword_1 & 0xFFFF) | (b.dword_0 << 16));
    out.push_back((b.dword_0 >> 16) | (b.dword_1 << 16));
  }
  for (const Instr& in : instrs) {
    out.insert(out.end(), in.dwords, in.dwords + 3);
  }
  return out;
}

// Number of control pairs for n control instructions = address
// of the first ALU/fetch instruction.
constexpr uint32_t FirstInstr(uint32_t cf_count) { return (cf_count + 1) / 2; }

// VS: vfetch r0.xyz1 <- vf0 (k_32_32_32_FLOAT, stride 3 dwords); mov oPos, r0.
inline std::vector<uint32_t> VsPositionFromStream() {
  using xe::gpu::xenos::VertexFormat;
  return Assemble(
      {CfAlloc(AllocType::kVsPosition, 0),
       CfExec(true, FirstInstr(2), 2, Seq({kSeqFetch, kSeqAluSerialize}))},
      {VertexFetch(0, FetchDstSwizzle(0, 1, 2, kFetchSwz1), 0, 0, VertexFormat::k_32_32_32_FLOAT,
                   3, 0),
       AluExportMov(ExportRegister::kVSPosition, R(0))});
}

// PS: mov oC0, c<constant> (default c0). Same length for every constant:
// only the src1_reg/src2_reg field of the ALU instruction changes.
inline std::vector<uint32_t> PsConstantColor(uint32_t constant = 0) {
  return Assemble({CfAlloc(AllocType::kPsColors, 0), CfExec(true, FirstInstr(2), 1, Seq({kSeqAlu}))},
                  {AluExportMov(ExportRegister::kPSColor0, C(constant))});
}

// VS: 5-dword vertex in vf<fetch_constant> (default vf0; xyz float
// position, xy float texcoord); vfetch r1.xy01 (offset 3), vfetch r0.xyz1
// (offset 0); mov oPos, r0; mov o0 (interpolator 0), r1. r0.x is the
// vertex index for both fetches, so the position (which
// overwrites it) is read last. With a texture in fetch constant 0 the
// vertex buffer goes elsewhere: the 6 dwords of texture 0 cover vf0, vf1 and vf2
// (2 dwords each), the first free one is vf3.
inline std::vector<uint32_t> VsPositionTexcoord(uint32_t fetch_constant = 0) {
  using xe::gpu::xenos::VertexFormat;
  constexpr uint32_t first = FirstInstr(4);
  return Assemble(
      {CfAlloc(AllocType::kVsPosition, 0),
       CfExec(false, first, 3, Seq({kSeqFetch, kSeqFetch, kSeqAluSerialize})),
       CfAlloc(AllocType::kVsInterpolators, 0),
       CfExec(true, first + 3, 1, Seq({kSeqAlu}))},
      {VertexFetch(1, FetchDstSwizzle(0, 1, kFetchSwz0, kFetchSwz1), 0, fetch_constant,
                   VertexFormat::k_32_32_FLOAT, 5, 3),
       VertexFetch(0, FetchDstSwizzle(0, 1, 2, kFetchSwz1), 0, fetch_constant,
                   VertexFormat::k_32_32_32_FLOAT, 5, 0),
       AluExportMov(ExportRegister::kVSPosition, R(0)),
       AluExportMov(ExportRegister::kVSInterpolator0, R(1))});
}

// PS: tfetch2D r0, r0.xy, tf0 (r0 = interpolator 0); mov oC0, r0.
inline std::vector<uint32_t> PsTexture2D() {
  return Assemble({CfAlloc(AllocType::kPsColors, 0),
                   CfExec(true, FirstInstr(2), 2, Seq({kSeqFetch, kSeqAluSerialize}))},
                  {TextureFetch2D(0, 0, 0), AluExportMov(ExportRegister::kPSColor0, R(0))});
}

// PS: tfetch2D r0, r0.xy, tf0; mov oC0, r0; mov oDepth, r0 (a depth-writing pass, like GoW3's
// occlusion query depth downsample).
inline std::vector<uint32_t> PsTexture2DWritingDepth() {
  return Assemble({CfAlloc(AllocType::kPsColors, 0),
                   CfExec(true, FirstInstr(2), 3, Seq({kSeqFetch, kSeqAluSerialize, kSeqAlu}))},
                  {TextureFetch2D(0, 0, 0), AluExportMov(ExportRegister::kPSColor0, R(0)),
                   AluExportMov(ExportRegister::kPSDepth, R(0))});
}

// VS: vfetch r0.xyz1 <- vf3 (texture 0 takes up vf0-vf2), tfetch2D r1, r0.xy, tf0 with gradients from a register (the only
// computed LOD possible in a VS); mov oPos, r1.
inline std::vector<uint32_t> VsTexture2DRegisterGradients() {
  using xe::gpu::xenos::VertexFormat;
  return Assemble(
      {CfAlloc(AllocType::kVsPosition, 0),
       CfExec(true, FirstInstr(2), 3, Seq({kSeqFetch, kSeqFetch, kSeqAluSerialize}))},
      {VertexFetch(0, FetchDstSwizzle(0, 1, 2, kFetchSwz1), 0, 3, VertexFormat::k_32_32_32_FLOAT, 3, 0),
       TextureFetch2D(1, 0, 0, 0, 0, /*register_gradients=*/true),
       AluExportMov(ExportRegister::kVSPosition, R(1))});
}

}  // namespace gow2::ucode_builder
