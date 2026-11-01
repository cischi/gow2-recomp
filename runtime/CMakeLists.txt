# gow2_runtime: platform (the only place that uses OS APIs), log,
# 4 GB guest arena and XEX image loader.
add_library(gow2_runtime STATIC
    platform/platform_win32.cpp
    platform/platform_win32_audio.cpp
    platform/platform_win32_display.cpp
    platform/platform_win32_input.cpp
    platform/sdl_gamepads.cpp
    log.cpp
    settings.cpp
    frame_pacing.cpp
    hdr_math.cpp
    engine_config.cpp
    game_patches/game_patches.cpp
    guest_memory.cpp
    physical_watch.cpp
    image_loader.cpp
    imports.cpp
    crash_map.cpp
    game_data.cpp)
target_include_directories(gow2_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gow2_runtime PRIVATE XenonUtils gow2_xex_check SDL3::SDL3-static tomlplusplus::tomlplusplus bcrypt)
target_compile_definitions(gow2_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# NVAPI excerpt (VRR): only for platform_win32_display.cpp, not exported to consumers.
target_include_directories(gow2_runtime PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/nvapi)

# APU: audio driver client, Audio Worker, WAV capture, XAudio* exports (then the XMA decoder).
add_subdirectory(apu)
add_subdirectory(hid)
# Kernel HLE: import implementations, marshalling, counters.
add_subdirectory(kernel)
# GPU: common module (registers, vblank, interrupts, Vd*) and fake backend (ring buffer, PM4).
add_subdirectory(gpu)
add_subdirectory(gpu_fake)
add_subdirectory(launcher)
add_subdirectory(dispatcher)

# ---- <id>.exe per game: recompiled code + import stubs + runtime ----
# gow2_add_game_exe(<id>): only if the game's recompile target has produced its generated
# folder (see cmake/Recompile.cmake), and anyway outside `all`: without game data the
# default build and tests do not touch it; it is built with --target <id>.
# Flags for the generated code (XenonRecomp requires clang; flags as in
# UnleashedRecomp): AVX2 for simde's VMX intrinsics, no strict aliasing
# assumption (guest memory is accessed with casts), warnings about the
# unused labels/variables that the generator emits.
set(_gow2_recomp_options
    /arch:AVX2
    /clang:-fno-strict-aliasing
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable)

function(gow2_add_game_exe id)
    string(TOUPPER "${id}" _ID)
    if(NOT ${_ID}_HAVE_GENERATED)
        return()
    endif()
    set(_generated "${${_ID}_GENERATED_DIR}")

    add_library(${id}_recomp OBJECT EXCLUDE_FROM_ALL ${${_ID}_GENERATED_SOURCES} "${${_ID}_IMPORT_STUBS}")
    add_dependencies(${id}_recomp ${id}_import_stubs)
    target_include_directories(${id}_recomp BEFORE PRIVATE "${_generated}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # only for simde's includes (ppc_context.h)
    target_link_libraries(${id}_recomp PRIVATE XenonUtils)
    target_compile_options(${id}_recomp PRIVATE ${_gow2_recomp_options})
    # The generated ppc_config.h includes "ppc_detail.h" (this folder) with
    # PPC_INCLUDE_DETAIL: PPC_MM_STORE_U32 with the XMA register hook.
    target_compile_definitions(${id}_recomp PRIVATE PPC_INCLUDE_DETAIL)
    # ppc_recomp_shared.h declares ~70,000 functions: precompiled only once
    target_precompile_headers(${id}_recomp PRIVATE "${_generated}/ppc_recomp_shared.h")

    add_executable(${id} EXCLUDE_FROM_ALL main.cpp recomp_glue.cpp)
    target_include_directories(${id} BEFORE PRIVATE "${_generated}")
    # gow2_gpu_d3d12 is defined in cmake/XeniaGpu.cmake (included after this folder).
    target_link_libraries(${id} PRIVATE ${id}_recomp gow2_gpu_d3d12 gow2_gpu_fake gow2_gpu gow2_apu gow2_hid gow2_kernel gow2_launcher ${id}_game gow2_runtime XenonUtils)
    target_compile_options(${id} PRIVATE ${_gow2_recomp_options})
    target_compile_definitions(${id} PRIVATE PPC_INCLUDE_DETAIL)
    # The recompiled code nests host calls as deeply as the game nests
    # guest ones: 16 MB main thread stack instead of 1 MB.
    # GUI subsystem: double-clicking opens no console; plat::SetupConsole attaches/allocates one on demand.
    target_link_options(${id} PRIVATE /STACK:0x1000000 /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
    # The launcher keeps a dxgi.dll proxy (OptiScaler) out of its process (main.cpp).
    target_link_options(${id} PRIVATE /DELAYLOAD:dxgi.dll /DELAYLOAD:d3d11.dll)
    target_link_libraries(${id} PRIVATE delayimp)
    # FidelityFX DLLs and licenses next to the exe (FSR, cmake/FidelityFX.cmake).
    gow2_copy_fidelityfx(${id})
    # DLSS bridge, nvngx_dlss.dll and licenses (only with the SDK, cmake/Dlss.cmake).
    gow2_copy_dlss(${id})
endfunction()

gow2_add_game_exe(gow2)
gow2_add_game_exe(gow3)
