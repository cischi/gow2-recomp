# gow2_gpu: GPU module common to the backends: GPU registers,
# 60 Hz vblank, FIFO interrupts, forwarding of the ring configuration to the
# backend and the Vd* exports (KERNEL_EXPORT). Compiles without the generated
# code, like gow2_kernel.
#
# tools/gen_import_stubs.py reads the KERNEL_EXPORTs also from the .cpp files of this
# folder (cmake/Recompile.cmake): every .cpp must be listed here.
#
# d3d12_probe.cpp: search for the hardware D3D12 adapter (loads dxgi.dll and
# d3d12.dll at runtime; Global Constraints exception for D3D12/DXGI).
# bmp.cpp, screenshot.cpp: 24-bit BMP screenshots and the --screenshot argument.
#
# presenter.cpp, d3d12_backend.cpp and shader_overrides.cpp include the Xenia
# headers: they live in gow2_gpu_d3d12 (cmake/XeniaGpu.cmake).
add_library(gow2_gpu STATIC
    bmp.cpp
    d3d12_probe.cpp
    dlss_fg_math.cpp
    frame_pacer.cpp
    gpu_system.cpp
    screenshot.cpp
    vd_exports.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/dof_downsample_4x4.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_motion_vectors.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_reactive.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/fg_hudless.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_reconstruct.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_vs.h"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_ps.h")
# The includes are "gpu/xxx.h", from the runtime/ folder.
target_include_directories(gow2_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# Headers of the replacement shaders compiled by fxc: "shaders/<name>.h".
target_include_directories(gow2_gpu PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gow2_gpu PUBLIC gow2_kernel PRIVATE gow2_runtime)

# Replacement shaders (runtime/gpu/shaders/*.hlsl): the Windows SDK's fxc (GOW2_FXC, cmake/XeniaShaders.cmake) compiles them
# into a header with the DXBC array.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/dof_downsample_4x4.h"
    COMMAND "${GOW2_FXC}" /nologo /enable_unbounded_descriptor_tables /T ps_5_1 /E main
            /Vn kDofDownsample4x4Dxbc /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/dof_downsample_4x4.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dof_downsample_4x4.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dof_downsample_4x4.hlsl"
    COMMENT "fxc dof_downsample_4x4.hlsl"
    VERBATIM)

# Upscaler passes (upscale_passes.cpp in gow2_gpu_d3d12): root signature in the bytecode.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_motion_vectors.h"
    COMMAND "${GOW2_FXC}" /nologo /T cs_5_1 /E main /Vn kUpscaleMotionVectorsCs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_motion_vectors.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_motion_vectors.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_motion_vectors.hlsl"
    COMMENT "fxc upscale_motion_vectors.hlsl"
    VERBATIM)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_reactive.h"
    COMMAND "${GOW2_FXC}" /nologo /T cs_5_1 /E main /Vn kUpscaleReactiveCs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/upscale_reactive.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_reactive.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_reactive.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gamma_ramp.hlsli"
    COMMENT "fxc upscale_reactive.hlsl"
    VERBATIM)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/fg_hudless.h"
    COMMAND "${GOW2_FXC}" /nologo /T cs_5_1 /E main /Vn kFgHudlessCs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/fg_hudless.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fg_hudless.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fg_hudless.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gamma_ramp.hlsli"
    COMMENT "fxc fg_hudless.hlsl"
    VERBATIM)

# HDR10 output (hdr_passes.cpp in gow2_gpu_d3d12): root signature in the bytecode (of the pixel shader for composition).
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_reconstruct.h"
    COMMAND "${GOW2_FXC}" /nologo /T cs_5_1 /E main /Vn kHdrReconstructCs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_reconstruct.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_reconstruct.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_reconstruct.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gamma_ramp.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/srgb.hlsli"
    COMMENT "fxc hdr_reconstruct.hlsl"
    VERBATIM)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_vs.h"
    COMMAND "${GOW2_FXC}" /nologo /T vs_5_1 /E vs_main /Vn kHdrComposeVs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_vs.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_compose.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_compose.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/srgb.hlsli"
    COMMENT "fxc hdr_compose.hlsl (vs_main)"
    VERBATIM)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_ps.h"
    COMMAND "${GOW2_FXC}" /nologo /T ps_5_1 /E ps_main /Vn kHdrComposePs
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/shaders/hdr_compose_ps.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_compose.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr_compose.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/srgb.hlsli"
    COMMENT "fxc hdr_compose.hlsl (ps_main)"
    VERBATIM)
