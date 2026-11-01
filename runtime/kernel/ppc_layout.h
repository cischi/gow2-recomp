// PPCContext layout that gow2_kernel, gow2_gpu and gow2_gpu_fake are compiled with.
//
// The two libraries include XenonUtils' ppc_context.h without the generated
// ppc_config.h (kernel/function.h defines PPC_CONFIG_H_INCLUDED), while the
// recompiled code uses build/generated/ppc_config.h: a skip_* / *_as_local
// option enabled in games/gow2/gow2.toml would remove fields from the generated
// code's PPCContext and the kernel would silently read r3, f1, lr... at the
// wrong offsets.
//
// The values below are literals: GOW2_CHECK_PPC_CONTEXT_LAYOUT() compares them
// with the PPCContext seen by the TU that expands it. It is expanded by
// kernel/function.h (so by every TU of the kernel, the fake GPU and the runtime
// that includes it) and by runtime/recomp_glue.cpp, which sees the generated
// ppc_config.h: if the two layouts diverge, one of the two sides fails to compile.
// CMake also rejects skip_* / *_as_local options in games/gow2/gow2.toml
// (cmake/Recompile.cmake) until the kernel supports them.
#pragma once

#include <cstddef>

namespace kernel::ppc_layout {

// sizeof and offsets of the fields the kernel reads or writes (marshalling of
// arguments r3..r10/f1..f13 and stack r1, return value r3/f1, lr, r13 of the
// PCR, fpscr of new threads).
inline constexpr std::size_t kKernelPPCContextSize = 2688;
inline constexpr std::size_t kOffsetR1 = 16;
inline constexpr std::size_t kOffsetR3 = 0;
inline constexpr std::size_t kOffsetR10 = 80;
inline constexpr std::size_t kOffsetR13 = 104;
inline constexpr std::size_t kOffsetF1 = 336;
inline constexpr std::size_t kOffsetF13 = 432;
inline constexpr std::size_t kOffsetLr = 256;
inline constexpr std::size_t kOffsetFpscr = 324;

}  // namespace kernel::ppc_layout

// To be expanded at namespace level after including ppc_context.h.
#define GOW2_CHECK_PPC_CONTEXT_LAYOUT()                                                                      \
  static_assert(sizeof(PPCContext) == ::kernel::ppc_layout::kKernelPPCContextSize,                          \
                "PPCContext: size differs from the one gow2_kernel uses (PPC_CONFIG_* options?)");           \
  static_assert(offsetof(PPCContext, r1) == ::kernel::ppc_layout::kOffsetR1, "PPCContext: offset of r1");     \
  static_assert(offsetof(PPCContext, r3) == ::kernel::ppc_layout::kOffsetR3, "PPCContext: offset of r3");     \
  static_assert(offsetof(PPCContext, r10) == ::kernel::ppc_layout::kOffsetR10, "PPCContext: offset of r10");  \
  static_assert(offsetof(PPCContext, r13) == ::kernel::ppc_layout::kOffsetR13, "PPCContext: offset of r13");  \
  static_assert(offsetof(PPCContext, f1) == ::kernel::ppc_layout::kOffsetF1, "PPCContext: offset of f1");     \
  static_assert(offsetof(PPCContext, f13) == ::kernel::ppc_layout::kOffsetF13, "PPCContext: offset of f13");  \
  static_assert(offsetof(PPCContext, lr) == ::kernel::ppc_layout::kOffsetLr, "PPCContext: offset of lr");     \
  static_assert(offsetof(PPCContext, fpscr) == ::kernel::ppc_layout::kOffsetFpscr, "PPCContext: offset of fpscr")
