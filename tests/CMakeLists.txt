gow2_add_test(sanity_test sanity_test.cpp)
gow2_add_test(xdvdfs_test xdvdfs_test.cpp xdvdfs_builder.cpp)
target_link_libraries(xdvdfs_test PRIVATE gow2_xdvdfs)
target_include_directories(xdvdfs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gow2_add_test(extract_cli_test extract_cli_test.cpp xdvdfs_builder.cpp)
target_link_libraries(extract_cli_test PRIVATE gow2_extract_lib)
target_include_directories(extract_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# "real ISO" (tens of seconds on the real ISO; skipped without an ISO) is part of the
# long suite: label `lungo`, applied after Catch2 test discovery.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extract_cli_labels.cmake"
    "set_tests_properties(\"real ISO\" PROPERTIES LABELS lungo)
")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_BINARY_DIR}/extract_cli_labels.cmake")

# Instructions added to XenonRecomp by patches/xenonrecomp: semantics of the
# ppc_context.h helpers and wiring of the operands in the emitter.
gow2_add_test(ppc_helpers_test ppc_helpers_test.cpp)
target_link_libraries(ppc_helpers_test PRIVATE XenonUtils)

# XMA register hook on PPC_MM_STORE_U32 (runtime/ppc_detail.h), with a fake
# gow2_mmio_store32: no gow2_kernel.
gow2_add_test(mmio_hook_test mmio_hook_test.cpp)
target_include_directories(mmio_hook_test PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
target_link_libraries(mmio_hook_test PRIVATE XenonUtils)

set(_xenon_recomp_dir "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/XenonRecomp")
gow2_add_test(recomp_emit_test recomp_emit_test.cpp
    "${_xenon_recomp_dir}/recompiler.cpp"
    "${_xenon_recomp_dir}/recompiler_config.cpp")
target_include_directories(recomp_emit_test PRIVATE "${_xenon_recomp_dir}")
# as in XenonRecomp: recompiler_config.cpp relies on pch.h being included implicitly
target_precompile_headers(recomp_emit_test PRIVATE "${_xenon_recomp_dir}/pch.h")
target_link_libraries(recomp_emit_test PRIVATE
    LibXenonAnalyse XenonUtils fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
target_compile_definitions(recomp_emit_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(recomp_emit_test PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic)

# Guest -> host translation with the offset of the 0xE0000000 view
# (runtime/guest_address.h, kernel::GuestPtr/GuestAddr): header only.
gow2_add_test(guest_address_test guest_address_test.cpp)
target_include_directories(guest_address_test PRIVATE "${CMAKE_SOURCE_DIR}/runtime")

# Minimal runtime. The [real] test compares the entry with config/expected_xex.json
# and loads game_data/default.xex (skipped if missing).
file(READ "${CMAKE_SOURCE_DIR}/config/expected_xex.json" _expected_xex_json)
string(JSON _expected_entry GET "${_expected_xex_json}" entry)
string(JSON _expected_import_count GET "${_expected_xex_json}" import_count)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/config/expected_xex.json")
gow2_add_test(runtime_test runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE gow2_runtime gow2_xex_check)
target_compile_definitions(runtime_test PRIVATE
    GOW2_EXPECTED_ENTRY=${_expected_entry}u
    GOW2_EXPECTED_IMPORT_COUNT=${_expected_import_count}
    GOW2_DEFAULT_XEX="${CMAKE_SOURCE_DIR}/game_data/default.xex")

# game_data: hash of default.xex, extraction marker, data folder resolution.
gow2_add_test(game_data_test game_data_test.cpp)
target_link_libraries(game_data_test PRIVATE gow2_runtime)

# launcher::CheckIso (ISO validation before extraction).
gow2_add_test(game_setup_test game_setup_test.cpp xdvdfs_builder.cpp)
target_link_libraries(game_setup_test PRIVATE gow2_launcher gow2_extract_lib)
target_include_directories(game_setup_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Host -> guest map of the crash handler, with a fake table.
gow2_add_test(crash_map_test crash_map_test.cpp)
target_link_libraries(crash_map_test PRIVATE gow2_runtime)

# Python tests (tools/, gen_import_stubs, smoke...): they skip themselves without game
# data or without the executables. Registered only if pytest is importable.
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest"
        RESULT_VARIABLE _pytest_rc OUTPUT_QUIET ERROR_QUIET)
    if(_pytest_rc EQUAL 0)
        # Short suite: everything except the tests marked `gioco` (they launch gow2.exe with the
        # game data for tens of seconds). Long suite: pytest_gioco, label `lungo`.
        add_test(NAME pytest
            COMMAND "${Python3_EXECUTABLE}" -m pytest "${CMAKE_SOURCE_DIR}/tests" -q -m "not gioco"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(NAME pytest_gioco
            COMMAND "${Python3_EXECUTABLE}" -m pytest "${CMAKE_SOURCE_DIR}/tests" -q -m gioco
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        # TIMEOUT: the long suite takes ~9 min (540 s) with the existing game tests plus ~12 min (735 s) of
        # test_milestone_frame_rate (4 runs of 180 s and audio analysis), i.e. ~1300 s; 3600 s leaves margin
        # for slower machines (ctest's default, 1500 s, would kill it).
        set_tests_properties(pytest_gioco PROPERTIES LABELS lungo TIMEOUT 3600)
    else()
        message(STATUS "pytest is not installed: Python tests not registered in CTest")
    endif()
endif()

# HLE kernel (runtime/kernel): argument marshalling and counters.
gow2_add_test(kernel_function_test kernel/function_test.cpp)
target_link_libraries(kernel_function_test PRIVATE gow2_kernel)
gow2_add_test(kernel_stats_test kernel/stats_test.cpp)
target_link_libraries(kernel_stats_test PRIVATE gow2_kernel gow2_runtime)
gow2_add_test(kernel_rtl_test kernel/rtl_test.cpp)
target_link_libraries(kernel_rtl_test PRIVATE gow2_kernel gow2_runtime)
gow2_add_test(kernel_memory_test kernel/memory_test.cpp)
target_link_libraries(kernel_memory_test PRIVATE gow2_kernel gow2_runtime)
gow2_add_test(kernel_sync_test kernel/sync_test.cpp)
target_link_libraries(kernel_sync_test PRIVATE gow2_kernel gow2_runtime)
# Guest threads; the crash in the stack guard runs in a child process
# without Catch2 (its exception handler would intercept the crash).
add_executable(kernel_threads_crash_child kernel/threads_crash_child.cpp)
target_link_libraries(kernel_threads_crash_child PRIVATE gow2_kernel gow2_runtime)
gow2_add_test(kernel_threads_test kernel/threads_test.cpp)
target_link_libraries(kernel_threads_test PRIVATE gow2_kernel gow2_runtime)
add_dependencies(kernel_threads_test kernel_threads_crash_child)
target_compile_definitions(kernel_threads_test PRIVATE
    GOW2_THREADS_CRASH_CHILD="$<TARGET_FILE:kernel_threads_crash_child>")
# Virtual file system (runtime/kernel/io): path resolution and the Nt*File exports.
gow2_add_test(kernel_vfs_test kernel/vfs_test.cpp)
target_link_libraries(kernel_vfs_test PRIVATE gow2_kernel gow2_runtime)
gow2_add_test(kernel_file_test kernel/file_test.cpp)
target_link_libraries(kernel_file_test PRIVATE gow2_kernel gow2_runtime)
# XAM (user, content, UI, notifications, language) and network/audio/XMA/voice stubs.
gow2_add_test(kernel_xam_test kernel/xam_test.cpp)
target_link_libraries(kernel_xam_test PRIVATE gow2_kernel gow2_runtime)
# Executable module (LDR, XEX header, sections) and kernel variables.
gow2_add_test(kernel_module_test kernel/module_test.cpp)
target_link_libraries(kernel_module_test PRIVATE gow2_kernel gow2_runtime)
# XMA decoder without a device: 0x7FEA0000 registers and the contexts array.
gow2_add_test(kernel_xma_test kernel/xma_test.cpp)
target_link_libraries(kernel_xma_test PRIVATE gow2_kernel gow2_runtime)

# Fake GPU (runtime/gpu_fake on the runtime/gpu module): PM4 interpreter on synthetic rings; ring buffer,
# write-back, vblank, interrupts and Vd* with the real threads.
gow2_add_test(gpu_fake_pm4_test gpu_fake/pm4_test.cpp)
target_link_libraries(gpu_fake_pm4_test PRIVATE gow2_gpu_fake gow2_kernel gow2_runtime)
gow2_add_test(gpu_fake_gpu_test gpu_fake/gpu_test.cpp)
target_link_libraries(gpu_fake_gpu_test PRIVATE gow2_gpu_fake gow2_kernel gow2_runtime)
# Common GPU module (runtime/gpu): backend selection, FIFO interrupts.
gow2_add_test(gpu_system_test gpu/gpu_system_test.cpp)
target_link_libraries(gpu_system_test PRIVATE gow2_gpu gow2_kernel gow2_runtime)
# Vendored Xenia GPU core (xenia_gpu_core): registers, tiling, ucode,
# DXBC translator, cvars and shim logs. C++20 like Xenia's code.
gow2_add_test(xenia_core_test gpu/xenia_core_test.cpp)
set_target_properties(xenia_core_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(xenia_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# xenia_gpu_d3d12 for the render_target_path_d3d12 cvar (its object pulls
# in the D3D12 backend and the CommandProcessor, hence also gow2_gpu_xenia).
target_link_libraries(xenia_core_test PRIVATE xenia_gpu_d3d12 xenia_gpu_core gow2_gpu_xenia gow2_gpu gow2_kernel gow2_runtime)
# Xenia's CommandProcessor (vendored, gow2 patches) on the guest ring and
# registers, without a GPU: NullCommandProcessor on gpu::CreateXeniaBackend.
gow2_add_test(xenia_cp_test gpu/xenia_cp_test.cpp)
target_include_directories(xenia_cp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia_cp_test PRIVATE gow2_gpu_xenia gow2_gpu gow2_kernel gow2_runtime)
# Hardware D3D12 adapter lookup (also runs without a GPU).
gow2_add_test(d3d12_probe_test gpu/d3d12_probe_test.cpp)
target_link_libraries(d3d12_probe_test PRIVATE gow2_gpu gow2_runtime)
# Vendored Xenia D3D12 backend (xenia_gpu_d3d12): provider and
# D3D12CommandProcessor on the guest ring. SKIP without a hardware adapter.
gow2_add_test(d3d12_cp_test gpu/d3d12_cp_test.cpp)
target_include_directories(d3d12_cp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_cp_test PRIVATE xenia_gpu_d3d12 gow2_gpu_xenia gow2_gpu gow2_kernel gow2_runtime)
# Game window (runtime/platform): thread with a message pump, WM_CLOSE. Run serially: the keyboard
# tests bring their window to the foreground and in parallel they would steal each other's focus.
add_executable(platform_window_test platform_window_test.cpp)
target_link_libraries(platform_window_test PRIVATE Catch2::Catch2WithMain gow2_runtime)
catch_discover_tests(platform_window_test PROPERTIES RESOURCE_LOCK gow2_window_focus)
# Settings from gow2.toml (runtime/settings): parser, errors, limits.
gow2_add_test(settings_test settings_test.cpp)
target_link_libraries(settings_test PRIVATE gow2_runtime)
# Game patches (runtime/game_patches): table, verification at startup on fake memory.
gow2_add_test(game_patches_test game_patches_test.cpp)
target_link_libraries(game_patches_test PRIVATE gow2_runtime XenonUtils)
# Launcher: model, ImGui panel without a backend, Win32 + D3D11 window (WARP if the GPU is missing).
gow2_add_test(launcher_test launcher_test.cpp)
target_link_libraries(launcher_test PRIVATE gow2_launcher gow2_gpu)
# NVIDIA DLSS runtime DLL download (runtime/launcher/nvidia_dlls): fake Fetch, no network.
gow2_add_test(nvidia_dlls_test nvidia_dlls_test.cpp)
target_link_libraries(nvidia_dlls_test PRIVATE gow2_launcher)
# OptiScaler (runtime/launcher/optiscaler): hash, detection, ini, installation with fake zip and fetch (no network).
# fake_optiscaler.dll: empty DLL with the version block of an arbitrary OptiScaler.
# The .rc goes through llvm-rc (next to clang-cl): CMake's RC rule with clang-cl + rc.exe hangs.
get_filename_component(GOW2_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(GOW2_LLVM_RC llvm-rc HINTS "${GOW2_LLVM_BIN}" REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fake_optiscaler.res
    COMMAND "${GOW2_LLVM_RC}" -no-preprocess -fo ${CMAKE_CURRENT_BINARY_DIR}/fake_optiscaler.res
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fake_optiscaler.rc
    DEPENDS fixtures/fake_optiscaler.rc)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/fake_optiscaler.res PROPERTIES EXTERNAL_OBJECT TRUE)
add_library(fake_optiscaler SHARED fixtures/fake_optiscaler.cpp ${CMAKE_CURRENT_BINARY_DIR}/fake_optiscaler.res)
gow2_add_test(optiscaler_test optiscaler_test.cpp)
target_link_libraries(optiscaler_test PRIVATE gow2_launcher)
target_compile_definitions(optiscaler_test PRIVATE GOW2_FAKE_OPTISCALER_DLL="$<TARGET_FILE:fake_optiscaler>")
add_dependencies(optiscaler_test fake_optiscaler)
# Non-Xbox pads with SDL3 (runtime/platform/sdl_gamepads): no pad required.
gow2_add_test(sdl_gamepads_test sdl_gamepads_test.cpp)
target_link_libraries(sdl_gamepads_test PRIVATE gow2_runtime)
# Screenshot without a GPU: --screenshot argument and 24-bit BMP (gow2_gpu).
gow2_add_test(screenshot_test gpu/screenshot_test.cpp)
target_include_directories(screenshot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(screenshot_test PRIVATE gow2_gpu gow2_runtime)
# Registry of the replacement shaders and layout check (CPU only;
# shader_overrides.cpp is in gow2_gpu_d3d12 because it includes Xenia).
gow2_add_test(shader_overrides_test gpu/shader_overrides_test.cpp)
target_link_libraries(shader_overrides_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
# DXGI presenter (gow2_gpu_d3d12) on an invisible window. SKIP without a
# hardware D3D12 adapter.
gow2_add_test(presenter_test gpu/presenter_test.cpp)
target_include_directories(presenter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(presenter_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
# --gpu d3d12 backend (gow2_gpu_d3d12): Xenia's D3D12CommandProcessor on the
# guest ring, presenter on an invisible window. SKIP without a hardware
# D3D12 adapter.
# The child loses the device (RemoveDevice) and exits with 5: without Catch2.
add_executable(d3d12_device_removed_child gpu/d3d12_device_removed_child.cpp)
target_link_libraries(d3d12_device_removed_child PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
gow2_add_test(d3d12_backend_test gpu/d3d12_backend_test.cpp)
target_include_directories(d3d12_backend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_backend_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
add_dependencies(d3d12_backend_test d3d12_device_removed_child)
target_compile_definitions(d3d12_backend_test PRIVATE
    GOW2_D3D12_DEVICE_REMOVED_CHILD="$<TARGET_FILE:d3d12_device_removed_child>")
# EDRAM of the d3d12 backend: clear and resolve on a synthetic ring
# (gpu/d3d12_harness.h, gpu/pm4_builder.h), rtv and rov paths. SKIP without a
# hardware D3D12 adapter; rov also without ROV.
gow2_add_test(d3d12_edram_test gpu/d3d12_edram_test.cpp)
target_include_directories(d3d12_edram_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_edram_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
gow2_add_test(d3d12_draw_test gpu/d3d12_draw_test.cpp)
target_include_directories(d3d12_draw_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_draw_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
gow2_add_test(d3d12_texture_test gpu/d3d12_texture_test.cpp)
target_include_directories(d3d12_texture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_texture_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
# MSAA 2x sample positions like the console (patch 0009): 2x depth from
# sample 0 at the pixel center, with the 2x draws' viewport shifted by -0.5 px in y.
gow2_add_test(d3d12_msaa_test gpu/d3d12_msaa_test.cpp)
target_include_directories(d3d12_msaa_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_msaa_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
# Replacement shaders (Xenia hook, patch 0010): fxc's DOF downsample
# against the CPU formula, on a synthetic guest shader with the same layout.
gow2_add_test(d3d12_shader_override_test gpu/d3d12_shader_override_test.cpp)
target_include_directories(d3d12_shader_override_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(d3d12_shader_override_test PRIVATE gow2_gpu_d3d12 gow2_gpu gow2_kernel gow2_runtime)
# Physical page watching (runtime/physical_watch) with the vectored
# write handler; the unwatched crash runs in a child process
# without Catch2.
add_executable(physical_watch_crash_child physical_watch_crash_child.cpp)
target_link_libraries(physical_watch_crash_child PRIVATE gow2_runtime)
gow2_add_test(physical_watch_test physical_watch_test.cpp)
target_include_directories(physical_watch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(physical_watch_test PRIVATE gow2_kernel gow2_runtime)
add_dependencies(physical_watch_test physical_watch_crash_child)
target_compile_definitions(physical_watch_test PRIVATE
    GOW2_PHYSWATCH_CRASH_CHILD="$<TARGET_FILE:physical_watch_crash_child>")

# Audio (runtime/apu, plat::AudioOut): paced null sink and WAV writer.
gow2_add_test(null_audio_out_test apu/null_audio_out_test.cpp)
target_link_libraries(null_audio_out_test PRIVATE gow2_runtime)
gow2_add_test(wav_writer_test apu/wav_writer_test.cpp)
target_include_directories(wav_writer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wav_writer_test PRIVATE gow2_apu gow2_runtime)
# Audio system (runtime/apu/audio_system.*): clients, Audio Worker, XAudio* exports.
gow2_add_test(audio_system_test apu/audio_system_test.cpp)
target_include_directories(audio_system_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_system_test PRIVATE gow2_apu gow2_kernel gow2_runtime)
# XAudio2 2.9 output (runtime/platform): timed cases SKIP without an audio endpoint.
gow2_add_test(xaudio2_out_test apu/xaudio2_out_test.cpp)
target_include_directories(xaudio2_out_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xaudio2_out_test PRIVATE gow2_runtime)
# Vendored FFmpeg subset (xenia_ffmpeg): xmaframes decoder.
gow2_add_test(ffmpeg_smoke_test apu/ffmpeg_smoke_test.cpp)
target_link_libraries(ffmpeg_smoke_test PRIVATE xenia_ffmpeg)
# Vendored Xenia APU (xenia_apu): XmaContext, XMA registers, BitStream.
# C++20 like Xenia's code; gow2_apu provides the hooks of the APU patches 0007/0008.
gow2_add_test(xenia_apu_test apu/xenia_apu_test.cpp)
set_target_properties(xenia_apu_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(xenia_apu_test PRIVATE xenia_apu gow2_apu gow2_kernel gow2_runtime)
# gow2::hooks glue of the APU (runtime/apu/xma_hooks.cpp): counters and log.
gow2_add_test(xma_hooks_test apu/xma_hooks_test.cpp)
target_include_directories(xma_hooks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xma_hooks_test PRIVATE gow2_apu gow2_kernel gow2_runtime)
# XMA decoder (runtime/apu/xma_decoder.*): XmaContext per slot, Kick/Lock/Clear, worker.
gow2_add_test(xma_decoder_test apu/xma_decoder_test.cpp)
target_include_directories(xma_decoder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xma_decoder_test PRIVATE gow2_apu gow2_kernel gow2_runtime)
# Input (runtime/hid/input_system.*): ordered drivers, XamInput* exports.
gow2_add_test(input_system_test hid/input_system_test.cpp)
target_include_directories(input_system_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hid)
target_link_libraries(input_system_test PRIVATE gow2_hid gow2_kernel gow2_runtime)
# --input-script (runtime/hid/input_script.*) and input drivers (runtime/hid/drivers.*).
gow2_add_test(input_script_test hid/input_script_test.cpp)
target_link_libraries(input_script_test PRIVATE gow2_hid gow2_kernel gow2_runtime)
gow2_add_test(drivers_test hid/drivers_test.cpp)
target_link_libraries(drivers_test PRIVATE gow2_hid gow2_kernel gow2_runtime)
gow2_add_test(sdl_driver_test hid/sdl_driver_test.cpp)
target_link_libraries(sdl_driver_test PRIVATE gow2_hid gow2_kernel gow2_runtime)
# Upscaler (runtime/gpu/upscaler.h): FSR with the FidelityFX DLLs copied next to the test.
# SKIP without the DLLs in the build or without a hardware D3D12 adapter.
gow2_add_test(upscaler_fsr_test gpu/upscaler_fsr_test.cpp)
target_include_directories(upscaler_fsr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(upscaler_fsr_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
gow2_copy_fidelityfx(upscaler_fsr_test)
# FSR frame generation (runtime/gpu/frame_generation.h): swap chain proxy with the DLLs copied next to the test.
# SKIP without the DLLs in the build or without a hardware D3D12 adapter.
gow2_add_test(frame_generation_fsr_test gpu/frame_generation_fsr_test.cpp)
target_include_directories(frame_generation_fsr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame_generation_fsr_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
gow2_copy_fidelityfx(frame_generation_fsr_test)
# DLSS (runtime/gpu/upscaler_dlss.h): adapter and jitter with the fake gow2_dlss.dll bridge (same ABI,
# no SDK) in its own folder.
add_library(gow2_dlss_fake SHARED gpu/fake_dlss_bridge.cpp)
target_include_directories(gow2_dlss_fake PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(gow2_dlss_fake PRIVATE d3d12)
set_target_properties(gow2_dlss_fake PROPERTIES OUTPUT_NAME gow2_dlss
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_dlss)
gow2_add_test(upscaler_dlss_test gpu/upscaler_dlss_test.cpp)
target_include_directories(upscaler_dlss_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(upscaler_dlss_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
# Real bridge (only with the SDK) in real_dlss/, not next to the tests: the other tests in the folder must
# keep seeing DLSS as unavailable.
target_compile_definitions(upscaler_dlss_test PRIVATE GOW2_FAKE_DLSS_DIR="${CMAKE_CURRENT_BINARY_DIR}/fake_dlss"
    GOW2_REAL_DLSS_DIR="${CMAKE_CURRENT_BINARY_DIR}/real_dlss")
add_dependencies(upscaler_dlss_test gow2_dlss_fake)
gow2_copy_dlss(upscaler_dlss_test ${CMAKE_CURRENT_BINARY_DIR}/real_dlss)
# DLSS frame generation (runtime/gpu/frame_generation_dlss.cpp) with the fake bridge: ring, timing thread, fallback.
gow2_add_test(frame_generation_dlss_test gpu/frame_generation_dlss_test.cpp)
target_include_directories(frame_generation_dlss_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame_generation_dlss_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
target_compile_definitions(frame_generation_dlss_test PRIVATE GOW2_FAKE_DLSS_DIR="${CMAKE_CURRENT_BINARY_DIR}/fake_dlss")
add_dependencies(frame_generation_dlss_test gow2_dlss_fake)
# Upscaler passes (runtime/gpu/upscale_passes.h): motion vectors (also with a still camera) and reactive mask
# with the gamma ramp on the GPU against the CPU formula; mip bias. SKIP without a hardware D3D12 adapter.
gow2_add_test(upscaler_test gpu/upscaler_test.cpp)
target_include_directories(upscaler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(upscaler_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
# Per-frame upscaler rules (runtime/gpu/upscale_frame_state.h): CPU only.
gow2_add_test(upscale_frame_state_test gpu/upscale_frame_state_test.cpp)
target_link_libraries(upscale_frame_state_test PRIVATE gow2_gpu_d3d12 gow2_runtime)

# Per-frame frame generation state (runtime/gpu/frame_generation_state.h): CPU only.
gow2_add_test(frame_generation_state_test gpu/frame_generation_state_test.cpp)
target_link_libraries(frame_generation_state_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
# Pacing per value, presenter limit and FrameLimiter (runtime/frame_pacing).
gow2_add_test(frame_pacing_test frame_pacing_test.cpp)
target_link_libraries(frame_pacing_test PRIVATE gow2_runtime)

# HDR10 output math (runtime/hdr_math).
gow2_add_test(hdr_math_test hdr_math_test.cpp)
target_link_libraries(hdr_math_test PRIVATE gow2_runtime)
# HDR passes on the GPU (runtime/gpu/hdr_passes.h) against hdr_math.
gow2_add_test(hdr_passes_test gpu/hdr_passes_test.cpp)
target_include_directories(hdr_passes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hdr_passes_test PRIVATE gow2_gpu_d3d12 gow2_runtime)

# Coalesced format, [SystemSettings] changes and generated files (runtime/engine_config).
gow2_add_test(engine_config_test engine_config_test.cpp)
target_link_libraries(engine_config_test PRIVATE gow2_runtime)

# Camera matrices for DLSS FG (runtime/gpu/dlss_fg_math.h): CPU only.
gow2_add_test(dlss_fg_math_test gpu/dlss_fg_math_test.cpp)
target_link_libraries(dlss_fg_math_test PRIVATE gow2_gpu_d3d12 gow2_runtime)

# Present timing for DLSS FG and pacing measurement (runtime/gpu/frame_pacer.h): CPU only.
gow2_add_test(frame_pacer_test gpu/frame_pacer_test.cpp)
target_link_libraries(frame_pacer_test PRIVATE gow2_gpu_d3d12 gow2_runtime)
