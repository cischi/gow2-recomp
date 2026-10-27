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
# Expected default.xex SHA-256 (game_data.h), from config/expected_xex.json.
file(READ "${CMAKE_SOURCE_DIR}/config/expected_xex.json" _gd_expected_json)
string(JSON _gd_xex_sha256 GET "${_gd_expected_json}" sha256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/config/expected_xex.json")
target_compile_definitions(gow2_runtime PUBLIC GOW2_XEX_SHA256="${_gd_xex_sha256}")
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

# ---- gow2.exe: recompiled code + import stubs + runtime ----
# Only if `recompile` has produced build/generated (see cmake/Recompile.cmake),
# and anyway outside `all`: without game data the default build and tests
# do not touch it; it is built with --target gow2.
if(GOW2_HAVE_GENERATED)
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

    add_library(gow2_recomp OBJECT EXCLUDE_FROM_ALL ${GOW2_GENERATED_SOURCES} "${GOW2_IMPORT_STUBS}")
    add_dependencies(gow2_recomp gow2_import_stubs)
    target_include_directories(gow2_recomp BEFORE PRIVATE "${GOW2_GENERATED_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # only for simde's includes (ppc_context.h)
    target_link_libraries(gow2_recomp PRIVATE XenonUtils)
    target_compile_options(gow2_recomp PRIVATE ${_gow2_recomp_options})
    # The generated ppc_config.h includes "ppc_detail.h" (this folder) with
    # PPC_INCLUDE_DETAIL: PPC_MM_STORE_U32 with the XMA register hook.
    target_compile_definitions(gow2_recomp PRIVATE PPC_INCLUDE_DETAIL)
    # ppc_recomp_shared.h declares ~70,000 functions: precompiled only once
    target_precompile_headers(gow2_recomp PRIVATE "${GOW2_GENERATED_DIR}/ppc_recomp_shared.h")

    add_executable(gow2 EXCLUDE_FROM_ALL main.cpp recomp_glue.cpp)
    target_include_directories(gow2 BEFORE PRIVATE "${GOW2_GENERATED_DIR}")
    # gow2_gpu_d3d12 is defined in cmake/XeniaGpu.cmake (included after this folder).
    target_link_libraries(gow2 PRIVATE gow2_recomp gow2_gpu_d3d12 gow2_gpu_fake gow2_gpu gow2_apu gow2_hid gow2_kernel gow2_launcher gow2_runtime XenonUtils)
    target_compile_options(gow2 PRIVATE ${_gow2_recomp_options})
    target_compile_definitions(gow2 PRIVATE PPC_INCLUDE_DETAIL)
    # The recompiled code nests host calls as deeply as the game nests
    # guest ones: 16 MB main thread stack instead of 1 MB.
    # GUI subsystem: double-clicking opens no console; plat::SetupConsole attaches/allocates one on demand.
    target_link_options(gow2 PRIVATE /STACK:0x1000000 /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
    # FidelityFX DLLs and licenses next to gow2.exe (FSR, cmake/FidelityFX.cmake).
    gow2_copy_fidelityfx(gow2)
    # DLSS bridge, nvngx_dlss.dll and licenses (only with the SDK, cmake/Dlss.cmake).
    gow2_copy_dlss(gow2)
endif()
