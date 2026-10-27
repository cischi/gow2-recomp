# xenia-canary APU (thirdparty/xenia, BSD-3-Clause; see thirdparty/xenia/VENDORED.md):
# XmaContextNew (default) and XmaContextOld (--xma-decoder old) on top of the abstract XmaContext base,
# with the xmaframes decoder from xenia_ffmpeg; XMA register table. Canary's XmaDecoder is not
# vendored: registers, workers and context allocation belong to gow2 (runtime/kernel/xma.cpp,
# runtime/apu).
#
# xenia_apu: the .cc files of src/xenia/apu/ from the thirdparty/xenia manifest (GOW2_XENIA_APU_SOURCES,
# collected by cmake/XeniaGpu.cmake; BitStream lives in xenia_gpu_core). C++20 like the GPU (same
# base headers: in C++17 byte_order.h would get a different std::endian, ODR), warnings off (-w).
# Include order: runtime/apu/shims (-I, our own) -> runtime/gpu/shims (-I, from
# xenia_gpu_core) -> thirdparty/xenia/src (SYSTEM, from xenia_gpu_core) -> thirdparty/ffmpeg
# (SYSTEM, from xenia_ffmpeg).
#
# Patches 0007/0008 (thirdparty/xenia/patches) call gow2::hooks::OnXmaFrameDecoded/
# OnXmaDecodeError (runtime/apu/shims/gow2_apu_hooks.h), implemented in gow2_apu (C++23, uses
# kernel::stats): anything that uses the contexts also links gow2_apu, like gow2_gpu_xenia for xenia_gpu_core.

set(GOW2_XENIA_APU_SHIMS_DIR "${CMAKE_SOURCE_DIR}/runtime/apu/shims")

set_source_files_properties(${GOW2_XENIA_APU_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
add_library(xenia_apu STATIC ${GOW2_XENIA_APU_SOURCES})
set_target_properties(xenia_apu PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(xenia_apu PUBLIC "${GOW2_XENIA_APU_SHIMS_DIR}")
target_compile_definitions(xenia_apu PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xenia_apu PUBLIC xenia_gpu_core xenia_ffmpeg)
