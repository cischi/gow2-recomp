cmake_minimum_required(VERSION 3.25)
project(gow2_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT everywhere: XenonRecomp sets /MT for its targets, and anything that links
# XenonUtils (xexdump, runtime) must use the same CRT (otherwise LNK2038).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MSVC toolset guard: Ninja reuses already compiled PCHs (cmake_pch.*) even if the
# STL / _MSC_FULL_VER changes (two VS installations, different vcvars) and the
# build breaks in obscure ways. VCToolsVersion is NOT enough (two VS installations
# can report the same version with different STLs): the toolset identity is the
# VCToolsInstallDir path + a hash of include/yvals_core.h (it contains
# _MSVC_STL_UPDATE/_MSVC_STL_VERSION, so it also tells in-place updates apart).
# It is stored on the first configure and the configure stops if it changes.
# It only runs at configure time: gow2.py build always reconfigures; with a manual
# `cmake --build` after a VS environment change, run `cmake --preset` first.
if(DEFINED ENV{VCToolsInstallDir} AND NOT "$ENV{VCToolsInstallDir}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _gow2_vcdir)
    string(TOLOWER "${_gow2_vcdir}" _gow2_vcdir)
    set(_gow2_stl_hash "")
    if(EXISTS "${_gow2_vcdir}/include/yvals_core.h")
        file(SHA1 "${_gow2_vcdir}/include/yvals_core.h" _gow2_stl_hash)
    endif()
    set(_gow2_toolset "${_gow2_vcdir}|${_gow2_stl_hash}")
    if(DEFINED GOW2_VCTOOLS AND NOT "${GOW2_VCTOOLS}" STREQUAL "${_gow2_toolset}")
        message(FATAL_ERROR
            "MSVC toolset changed (${GOW2_VCTOOLS} -> ${_gow2_toolset}): the PCHs in "
            "${CMAKE_BINARY_DIR} were compiled with the other toolset. Delete "
            "${CMAKE_BINARY_DIR} (build/clang-release; NOT build/generated) and reconfigure; "
            "to always use the same environment: python tools/gow2.py shell cmake --preset clang-release")
    endif()
    set(GOW2_VCTOOLS "${_gow2_toolset}" CACHE INTERNAL "Identity of the MSVC toolset used for this build")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

enable_testing()
include(cmake/Catch2.cmake)

# Local patches to the submodule (instructions missing upstream), before using it
include(cmake/XenonRecompPatches.cmake)
add_subdirectory(thirdparty/XenonRecomp)
add_subdirectory(common/xex_check)
# `recompile` target (not in `all`), GOW2_GENERATED_SOURCES and the import
# stubs: before runtime/, which derives the gow2 executable from them
include(cmake/Recompile.cmake)
# Static SDL3 (thirdparty/SDL) for non-Xbox pads: SDL3::SDL3-static
include(cmake/SDL.cmake)
# Dear ImGui (thirdparty/imgui, win32 + dx11 backends) for the launcher: gow2_imgui
include(cmake/ImGui.cmake)
# fxc (GOW2_FXC) and gow2_xenia_dxbc_shaders: before runtime/ and XeniaGpu
include(cmake/XeniaShaders.cmake)
# FidelityFX SDK (headers, downloaded DLLs, gow2_copy_fidelityfx): before runtime/
include(cmake/FidelityFX.cmake)
# DLSS bridge gow2_dlss.dll (only with GOW2_DLSS_SDK_DIR), gow2_copy_dlss: before runtime/
include(cmake/Dlss.cmake)
add_subdirectory(runtime)
# Vendored Xenia GPU code (thirdparty/xenia) and shims: xenia_gpu_core
include(cmake/XeniaGpu.cmake)
# FFmpeg subset of xenia-canary (has207 fork, thirdparty/ffmpeg): xenia_ffmpeg
include(cmake/FFmpeg.cmake)
# xenia-canary APU (XmaContextNew/Old, XMA registers) in thirdparty/xenia: xenia_apu
include(cmake/XeniaApu.cmake)
add_subdirectory(tests)
add_subdirectory(tools/extract)
add_subdirectory(tools/xexdump)
