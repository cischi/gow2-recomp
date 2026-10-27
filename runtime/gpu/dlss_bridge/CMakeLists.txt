# SPDX-License-Identifier: MIT (see LICENSE.txt in this folder)
# gow2_dlss.dll: DLSS bridge (NGX). Inside the gow2 project it is included by cmake/Dlss.cmake with
# GOW2_DLSS_SDK_DIR; standalone: cmake -S runtime/gpu/dlss_bridge -B <build> -DDLSS_SDK_ROOT=<NVIDIA/DLSS checkout>.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gow2_dlss LANGUAGES CXX)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT DLSS_SDK_ROOT)
    message(FATAL_ERROR "gow2_dlss: DLSS_SDK_ROOT not set (checkout of https://github.com/NVIDIA/DLSS)")
endif()

# nvsdk_ngx_s.lib: static CRT (/MT), like the rest of gow2.
add_library(gow2_dlss SHARED gow2_dlss_bridge.cpp)
target_compile_features(gow2_dlss PRIVATE cxx_std_17)
target_include_directories(gow2_dlss PRIVATE "${DLSS_SDK_ROOT}/include")
target_link_libraries(gow2_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" d3d12)
# The NVIDIA headers are not our code: no warnings from them.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gow2_dlss PRIVATE -Wno-everything)
endif()
