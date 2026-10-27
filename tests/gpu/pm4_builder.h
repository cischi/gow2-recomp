// Builder of synthetic PM4 rings for the GPU tests: register writes
// (type 0 packets), EDRAM surface, resolve as the Xbox 360's Direct3D 9
// emits them (RB_COPY_* registers + 3-vertex draw with
// RB_MODECONTROL = kCopy), MEM_WRITE fence.
//
// Register fields from thirdparty/xenia/src/xenia/gpu/registers.h and indices from
// register_table.inc (Xenia @ 95a5c3ee, BSD-3-Clause). The resolve
// rectangle is read by draw_util::GetResolveInfo from vertex fetch constant 0
// (3 vertices x 2 floats, k8in32), with the minimum and maximum of the coordinates.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "gpu/xenos.h"
#include "kernel/guest.h"

namespace gow2_test {

// Register indices (register_table.inc).
inline constexpr uint32_t kRegRbSurfaceInfo = 0x2000;
inline constexpr uint32_t kRegRbColorInfo = 0x2001;
inline constexpr uint32_t kRegRbDepthInfo = 0x2002;
inline constexpr uint32_t kRegPaScScreenScissorTl = 0x200E;
inline constexpr uint32_t kRegPaScScreenScissorBr = 0x200F;
inline constexpr uint32_t kRegPaScWindowOffset = 0x2080;
inline constexpr uint32_t kRegPaScWindowScissorTl = 0x2081;
inline constexpr uint32_t kRegPaScWindowScissorBr = 0x2082;
inline constexpr uint32_t kRegPaSuScModeCntl = 0x2205;
inline constexpr uint32_t kRegRbModecontrol = 0x2208;
inline constexpr uint32_t kRegPaSuVtxCntl = 0x2302;
inline constexpr uint32_t kRegRbCopyControl = 0x2318;  // .. DEST_BASE, DEST_PITCH, DEST_INFO
inline constexpr uint32_t kRegRbColorClear = 0x231E;   // .. RB_COLOR_CLEAR_LO
inline constexpr uint32_t kRegVgtMaxVtxIndx = 0x2100;  // .. VGT_MIN_VTX_INDX, VGT_INDX_OFFSET
inline constexpr uint32_t kRegRbColorMask = 0x2104;
inline constexpr uint32_t kRegSqProgramCntl = 0x2180;
inline constexpr uint32_t kRegRbDepthcontrol = 0x2200;  // .. RB_BLENDCONTROL0, RB_COLORCONTROL
inline constexpr uint32_t kRegPaClClipCntl = 0x2204;
inline constexpr uint32_t kRegPaClVteCntl = 0x2206;

// Type 3 opcodes (xenos.h, Type3Opcode) not yet in runtime/gpu/xenos.h.
inline constexpr uint32_t kPm4ImLoad = 0x27;
inline constexpr uint32_t kPm4ImLoadImmediate = 0x2B;
inline constexpr uint32_t kPm4LoadAluConstant = 0x2F;
inline constexpr uint32_t kPm4DrawIndx2 = 0x36;

// xenos.h ShaderType, PrimitiveType, SourceSelect.
inline constexpr uint32_t kShaderVertex = 0, kShaderPixel = 1;
inline constexpr uint32_t kPrimRectangleList = 8;
inline constexpr uint32_t kSourceDma = 0, kSourceAutoIndex = 2;
// ALU constants: type 0 SET_CONSTANT/LOAD_ALU_CONSTANT add 0x4000;
// the PS floats start at c256 (4 dwords per constant).
inline constexpr uint32_t kAluConstantPs0 = 256 * 4;

// xenos.h
inline constexpr uint32_t kMsaa1X = 0, kMsaa2X = 1, kMsaa4X = 2;
inline constexpr uint32_t kColorRt8888 = 0;      // ColorRenderTargetFormat::k_8_8_8_8
inline constexpr uint32_t kColorFormat8888 = 6;  // ColorFormat::k_8_8_8_8
inline constexpr uint32_t kEndianNone = 0, kEndian8in16 = 1, kEndian8in32 = 2, kEndian16in32 = 3;
inline constexpr uint32_t kEdramModeColorDepth = 4, kEdramModeCopy = 6;

class Pm4Builder {
 public:
  // vertex_area: guest address of physical memory (>= 32 bytes for each
  // ResolveRect of the ring) where ResolveRect writes the vertices with the CPU.
  explicit Pm4Builder(uint32_t vertex_area = 0) : vertex_area_(vertex_area) {}

  const std::vector<uint32_t>& words() const { return words_; }
  void Clear() { words_.clear(); }

  // Type 0: values in consecutive registers starting from first.
  Pm4Builder& SetRegs(uint32_t first, std::initializer_list<uint32_t> values) {
    words_.push_back(gpu::MakePacketType0(first, uint32_t(values.size())));
    words_.insert(words_.end(), values.begin(), values.end());
    return *this;
  }

  // Color render target 0 in EDRAM: RB_SURFACE_INFO (pitch in pixels,
  // MSAA), RB_COLOR_INFO (base in tiles, format), no window offset,
  // window and screen scissors open (0,0)-(8192,8192), like D3D9
  // (draw_util::GetScissor collapses the rectangle with scissors at 0).
  Pm4Builder& SetSurface(uint32_t pitch, uint32_t msaa, uint32_t color_format, uint32_t edram_base) {
    SetRegs(kRegRbSurfaceInfo, {(pitch & 0x3FFF) | (msaa & 3) << 16, (edram_base & 0xFFF) | (color_format & 0xF) << 16,
                                0});
    SetRegs(kRegPaScScreenScissorTl, {0, 8192u | 8192u << 16});
    SetRegs(kRegPaScWindowOffset, {0, 1u << 31 /* window_offset_disable */, 8192u | 8192u << 16});
    SetRegs(kRegPaSuScModeCntl, {0});
    return *this;
  }

  // Rectangle [x0,x1) x [y0,y1) of the resolve: 3 vertices (x0,y0) (x1,y0)
  // (x1,y1) as big-endian floats in vertex_area, vertex fetch constant 0 of
  // type kVertex (3), size 6 dwords, endian k8in32. PA_SU_VTX_CNTL.pix_center
  // = kOGLHalf: no half pixel added, the coordinates are the edges.
  Pm4Builder& ResolveRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t at = vertex_area_ + vertex_slots_++ * 32;
    const float v[6] = {float(x0), float(y0), float(x1), float(y0), float(x1), float(y1)};
    for (int i = 0; i < 6; ++i) *kernel::GuestPtr<uint32_t>(at + i * 4) = std::byteswap(std::bit_cast<uint32_t>(v[i]));
    const uint32_t phys = at & 0x1FFFFFFF;
    SetRegs(gpu::kRegShaderConstantFetch00, {phys | 3u, (6u << 2) | kEndian8in32});
    SetRegs(kRegPaSuVtxCntl, {1});
    return *this;
  }

  // Resolve of color render target 0 (copy_src_select 0, sample 0,
  // copy_command kRaw) toward dest_phys, tiled 2D texture dest_pitch x
  // dest_height, format dest_format; clear: also color_clear_enable with
  // RB_COLOR_CLEAR = RB_COLOR_CLEAR_LO = clear_color. Then auto-index DRAW_INDX
  // of 3 indices (rectangle list) with RB_MODECONTROL = kCopy: the
  // command processor calls IssueCopy. swap_red_blue: RB_COPY_DEST_INFO.
  // copy_dest_swap (red and blue swapped, like D3D9 for ARGB
  // destinations). copy_src_select: 0-3 color, 4 depth (sample 0).
  Pm4Builder& Resolve(uint32_t dest_phys, uint32_t dest_pitch, uint32_t dest_height, uint32_t dest_endian,
                      uint32_t clear_color, bool clear, uint32_t dest_format = kColorFormat8888,
                      bool swap_red_blue = false, uint32_t copy_src_select = 0) {
    SetRegs(kRegRbModecontrol, {kEdramModeCopy});
    SetRegs(kRegRbCopyControl, {(clear ? 1u << 8 : 0u) | (copy_src_select & 7), dest_phys & 0x1FFFFFFF,
                                (dest_pitch & 0x3FFF) | (dest_height & 0x3FFF) << 16,
                                (dest_endian & 7) | (dest_format & 0x3F) << 7 | uint32_t(swap_red_blue) << 24});
    SetRegs(kRegRbColorClear, {clear_color, clear_color});
    // VGT_DRAW_INITIATOR: prim_type kRectangleList (8), source_select
    // kAutoIndex (2), num_indices 3.
    words_.push_back(gpu::MakePacketType3(gpu::kPm4DrawIndx, 2));
    words_.push_back(0);  // viz query
    words_.push_back(8u | 2u << 6 | 3u << 16);
    SetRegs(kRegRbModecontrol, {kEdramModeColorDepth});
    return *this;
  }

  // Draw state of a rectangle in screen coordinates in color render target
  // 0 (k_8_8_8_8, opaque): RB_MODECONTROL = kColorDepth, RGBA write,
  // no depth, ONE/ZERO blend (0x00010001, no blending),
  // PA_CL_CLIP_CNTL.clip_disable, PA_CL_VTE_CNTL with only vtx_xy_fmt,
  // vtx_z_fmt and vtx_w0_fmt (no viewport scale/offset: the positions
  // are already in pixels), PA_SU_VTX_CNTL.pix_center = kOGLHalf (no half
  // pixel), SQ_PROGRAM_CNTL = 0 (1 register, export kPosition1Vector),
  // indices 0..0xFFFFFF without offset.
  Pm4Builder& ScreenSpaceDrawState() {
    SetRegs(kRegRbModecontrol, {kEdramModeColorDepth});
    SetRegs(kRegRbColorMask, {0xF});
    SetRegs(kRegRbDepthcontrol, {0, 0x00010001, 0});
    SetRegs(kRegPaClClipCntl, {1u << 16});
    SetRegs(kRegPaClVteCntl, {1u << 8 | 1u << 9 | 1u << 10});
    SetRegs(kRegPaSuVtxCntl, {1});
    SetRegs(kRegSqProgramCntl, {0});
    SetRegs(kRegVgtMaxVtxIndx, {0xFFFFFF, 0, 0});
    return *this;
  }

  // Generic type 3 packet.
  Pm4Builder& Type3(uint32_t opcode, std::initializer_list<uint32_t> payload) {
    words_.push_back(gpu::MakePacketType3(opcode, uint32_t(payload.size())));
    words_.insert(words_.end(), payload.begin(), payload.end());
    return *this;
  }

  // IM_LOAD_IMMEDIATE: microcode (logical dwords, written big-endian in the ring
  // like every dword) inside the packet.
  Pm4Builder& ImLoadImmediate(uint32_t shader_type, const std::vector<uint32_t>& ucode) {
    words_.push_back(gpu::MakePacketType3(kPm4ImLoadImmediate, 2 + uint32_t(ucode.size())));
    words_.push_back(shader_type);
    words_.push_back(uint32_t(ucode.size()));  // start 0, size in dwords
    words_.insert(words_.end(), ucode.begin(), ucode.end());
    return *this;
  }

  // IM_LOAD: big-endian microcode in physical memory at phys.
  Pm4Builder& ImLoad(uint32_t shader_type, uint32_t phys, uint32_t size_dwords) {
    return Type3(kPm4ImLoad, {(phys & 0x1FFFFFFC) | (shader_type & 3), size_dwords & 0xFFFF});
  }

  // ALU-type SET_CONSTANT: values starting from the dword index (0x4000 + index).
  Pm4Builder& SetAluConstants(uint32_t index, std::initializer_list<uint32_t> values) {
    words_.push_back(gpu::MakePacketType3(gpu::kPm4SetConstant, 1 + uint32_t(values.size())));
    words_.push_back(index & 0x7FF);  // type 0 (ALU) in bits 16-23
    words_.insert(words_.end(), values.begin(), values.end());
    return *this;
  }

  // LOAD_ALU_CONSTANT: size_dwords big-endian dwords from phys to 0x4000 + index.
  Pm4Builder& LoadAluConstants(uint32_t phys, uint32_t index, uint32_t size_dwords) {
    return Type3(kPm4LoadAluConstant, {phys & 0x1FFFFFFF, index & 0x7FF, size_dwords & 0xFFF});
  }

  // Vertex fetch constant 0 (kVertex) over size_dwords big-endian dwords
  // (k8in32) at phys.
  Pm4Builder& VertexFetch0(uint32_t phys, uint32_t size_dwords) {
    return SetRegs(gpu::kRegShaderConstantFetch00, {(phys & 0x1FFFFFFC) | 3u, (size_dwords << 2) | kEndian8in32});
  }

  // Vertex fetch constant `index` (kVertex) over size_dwords big-endian dwords
  // (k8in32) at phys: register SHADER_CONSTANT_FETCH_00 + 2 * index.
  Pm4Builder& VertexFetch(uint32_t index, uint32_t phys, uint32_t size_dwords) {
    words_.push_back(gpu::MakePacketType0(gpu::kRegShaderConstantFetch00 + 2 * index, 2));
    words_.push_back((phys & 0x1FFFFFFC) | 3u);
    words_.push_back((size_dwords << 2) | kEndian8in32);
    return *this;
  }

  // Texture fetch constant `index` (6 dwords, xenos.h xe_gpu_texture_fetch_t):
  // registers SHADER_CONSTANT_FETCH_00 + 6 * index.
  Pm4Builder& TextureFetch(uint32_t index, const std::array<uint32_t, 6>& dwords) {
    words_.push_back(gpu::MakePacketType0(gpu::kRegShaderConstantFetch00 + 6 * index, 6));
    words_.insert(words_.end(), dwords.begin(), dwords.end());
    return *this;
  }

  // Auto-index DRAW_INDX_2 (without viz query).
  Pm4Builder& DrawIndx2Auto(uint32_t prim_type, uint32_t num_indices) {
    return Type3(kPm4DrawIndx2, {prim_type | kSourceAutoIndex << 6 | num_indices << 16});
  }

  // DRAW_INDX with 16-bit indices in physical memory (VGT_DMA_BASE/SIZE,
  // swap_mode = endian).
  Pm4Builder& DrawIndx16(uint32_t prim_type, uint32_t num_indices, uint32_t index_phys, uint32_t endian) {
    return Type3(gpu::kPm4DrawIndx, {0, prim_type | kSourceDma << 6 | 0u << 11 /* kInt16 */ | num_indices << 16,
                                     index_phys & 0x1FFFFFFE, (num_indices & 0xFFFFFF) | (endian & 3) << 30});
  }

  // MEM_WRITE of value to phys (endian k8in32: big-endian in guest memory).
  Pm4Builder& Fence(uint32_t phys, uint32_t value) {
    words_.push_back(gpu::MakePacketType3(gpu::kPm4MemWrite, 2));
    words_.push_back((phys & 0x1FFFFFFC) | kEndian8in32);
    words_.push_back(value);
    return *this;
  }

 private:
  uint32_t vertex_area_;
  uint32_t vertex_slots_ = 0;
  std::vector<uint32_t> words_;
};

}  // namespace gow2_test
