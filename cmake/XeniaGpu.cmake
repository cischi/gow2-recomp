# Vendored Xenia GPU code (thirdparty/xenia, BSD-3-Clause; see
# thirdparty/xenia/VENDORED.md) with the shims in runtime/gpu/shims.
#
# xenia_gpu_core: GPU core without D3D12 (registers, ucode, shader
# analysis, DXBC translator, generic texture/render target/primitive caches,
# DXBC checksum, generic CommandProcessor). No dependency on
# runtime/kernel, except command_processor.cc: it uses GraphicsSystem, XHostThread
# and gow2::hooks, implemented in gow2_gpu_xenia (below). Anything that uses the
# CommandProcessor links gow2_gpu_xenia; for the others the object is not needed (the
# library is static).
#
# gow2_gpu_xenia: bridge between Xenia's code and the runtime (C++23 like the
# rest of gow2): GraphicsSystem and XThread shims on top of the common GPU module and the
# guest threads, gpu::CreateXeniaBackend backend, gow2::hooks hooks.
#
# The vendored sources are the .cc/.cpp files of thirdparty/xenia/manifest.txt
# (tools/vendor_xenia.py copies exactly that list). They build as C++20
# like in xenia-canary, with warnings off (-w); the shims instead build with the
# project's normal warnings. Xenia's includes are SYSTEM (/imsvc),
# so they come after runtime/gpu/shims (-I): a shim always wins over the
# file of the same name, which must not exist anyway (test_vendor_xenia.py).

set(GOW2_XENIA_DIR "${CMAKE_SOURCE_DIR}/thirdparty/xenia")
set(GOW2_XENIA_SHIMS_DIR "${CMAKE_SOURCE_DIR}/runtime/gpu/shims")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOW2_XENIA_DIR}/manifest.txt")
file(STRINGS "${GOW2_XENIA_DIR}/manifest.txt" _xenia_manifest REGEX "^[^#].*\\.(cc|cpp)$")
# Sources under a d3d12/ folder (gpu/d3d12, ui/d3d12) go to
# xenia_gpu_d3d12, those in src/xenia/apu/ to xenia_apu (cmake/XeniaApu.cmake,
# GOW2_XENIA_APU_SOURCES), the others to xenia_gpu_core.
set(_xenia_core_sources "")
set(_xenia_d3d12_sources "")
set(GOW2_XENIA_APU_SOURCES "")
foreach(_rel IN LISTS _xenia_manifest)
    string(STRIP "${_rel}" _rel)
    if(_rel MATCHES "^src/xenia/apu/")
        list(APPEND GOW2_XENIA_APU_SOURCES "${GOW2_XENIA_DIR}/${_rel}")
    elseif(_rel MATCHES "/d3d12/")
        list(APPEND _xenia_d3d12_sources "${GOW2_XENIA_DIR}/${_rel}")
    else()
        list(APPEND _xenia_core_sources "${GOW2_XENIA_DIR}/${_rel}")
    endif()
endforeach()
set_source_files_properties(${_xenia_core_sources} PROPERTIES COMPILE_OPTIONS "-w")

set(_xenia_core_shims
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/clock.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/cvar.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/filesystem.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/logging.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/memory_host.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/mutex.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/platform_amd64.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/profiling.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/string.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/base/threading.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/memory.cpp")

add_library(xenia_gpu_core STATIC ${_xenia_core_sources} ${_xenia_core_shims})
# C++20 like xenia-canary (CMakeLists.txt: CMAKE_CXX_STANDARD 20). Anything that
# includes Xenia's headers uses C++20 or later.
set_target_properties(xenia_gpu_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(xenia_gpu_core PUBLIC "${GOW2_XENIA_SHIMS_DIR}")
target_include_directories(xenia_gpu_core SYSTEM PUBLIC "${GOW2_XENIA_DIR}/src" "${GOW2_XENIA_DIR}")
target_compile_definitions(xenia_gpu_core PUBLIC UNICODE _UNICODE)
# Xenia's headers have inline functions with AVX2 intrinsics: same
# options for whoever includes them (ODR).
target_compile_options(xenia_gpu_core PUBLIC /arch:AVX2)
# Standard headers that canary's code gets from submodules that are not
# vendored (runtime/gpu/shims/gow2_canary_std.h): PUBLIC, also needed by
# whoever includes Xenia's headers.
target_compile_options(xenia_gpu_core PUBLIC "/FI${GOW2_XENIA_SHIMS_DIR}/gow2_canary_std.h")
# _mm_cvtsi64x_si128 (canary's draw_util.cc, XE_PLATFORM_WIN32 branch) is an
# MSVC alias that clang-cl lacks: same instruction as _mm_cvtsi64_si128.
target_compile_definitions(xenia_gpu_core PRIVATE _CRT_SECURE_NO_WARNINGS _mm_cvtsi64x_si128=_mm_cvtsi64_si128)
target_link_libraries(xenia_gpu_core PUBLIC fmt::fmt xxHash::xxhash PRIVATE gow2_runtime)

add_library(gow2_gpu_xenia STATIC
    "${CMAKE_SOURCE_DIR}/runtime/gpu/xenia_backend.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/gpu/graphics_system.cpp"
    "${GOW2_XENIA_SHIMS_DIR}/xenia/kernel/xthread.cpp")
target_link_libraries(gow2_gpu_xenia PUBLIC xenia_gpu_core gow2_gpu gow2_kernel PRIVATE gow2_runtime)

# xenia_gpu_d3d12: Xenia's D3D12 backend (D3D12CommandProcessor, pipeline/
# texture/render target caches, shared memory, shaders in
# gpu/shaders/bytecode/d3d12_5_1 compiled by fxc at build time:
# cmake/XeniaShaders.cmake) and D3D12Provider with patch 0006 (no
# presenter, no dxilconv/dxcompiler, no realtime priority). The
# shims it uses are headers (d3d12_graphics_system.h, d3d12_presenter.h,
# DxbcConverter.h). d3d12.dll/dxgi.dll/D3DCompiler_47.dll are loaded at
# runtime (D3D12Provider::Initialize); dxguid.lib provides the IIDs/GUIDs of
# D3D12/DXGI. Anything that creates the D3D12CommandProcessor also links gow2_gpu_xenia
# (GraphicsSystem, XHostThread), as for xenia_gpu_core.
add_library(xenia_gpu_d3d12 STATIC ${_xenia_d3d12_sources})
# clang treats "address of temporary" as an error even with -w; MSVC (which
# Xenia builds with) accepts it as an extension: &provider.OffsetRTVDescriptor(...)
# in d3d12_render_target_cache.cc, valid until the end of the expression.
set_source_files_properties(${_xenia_d3d12_sources} PROPERTIES COMPILE_OPTIONS "-w;-Wno-address-of-temporary")
set_target_properties(xenia_gpu_d3d12 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(xenia_gpu_d3d12 PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xenia_gpu_d3d12 PUBLIC xenia_gpu_core dxguid)
gow2_xenia_dxbc_shaders(xenia_gpu_d3d12 "${GOW2_XENIA_DIR}/src/xenia/gpu/shaders" xenia/gpu)

# gow2_gpu_d3d12: gow2's --gpu d3d12 backend (runtime/gpu/d3d12_backend.h) and
# DXGI presenter with screenshots (runtime/gpu/presenter.h, derived from the
# D3D12Presenter shim). C++23 like the rest of gow2. It links Xenia's D3D12
# backend (provider and D3D12CommandProcessor, which runs the game's ring) and the
# gow2_gpu_xenia bridge (GraphicsSystem, CreateXeniaBackend).
add_library(gow2_gpu_d3d12 STATIC
    "${CMAKE_SOURCE_DIR}/runtime/gpu/d3d12_backend.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/overlay.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/presenter.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/shader_overrides.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/upscale_frame_state.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/frame_generation_state.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/frame_generation_fsr.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/frame_generation_dlss.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/upscale_passes.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/hdr_passes.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/dlss_bridge_loader.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/upscaler_dlss.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/gpu/upscaler_fsr.cpp")
target_link_libraries(gow2_gpu_d3d12 PUBLIC xenia_gpu_d3d12 gow2_gpu_xenia gow2_gpu gow2_kernel
    gow2_runtime gow2_launcher PRIVATE gow2_fidelityfx)
# Presenter shaders (guest_output_*, xenia/ui/shaders).
gow2_xenia_dxbc_shaders(gow2_gpu_d3d12 "${GOW2_XENIA_DIR}/src/xenia/ui/shaders" xenia/ui)
