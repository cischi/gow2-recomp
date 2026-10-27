# DLSS bridge: gow2_dlss.dll (MIT, runtime/gpu/dlss_bridge) is only built with the NVIDIA SDK,
# which is not part of the repository.
#
# GOW2_DLSS_SDK_DIR: checkout of https://github.com/NVIDIA/DLSS (tag v310.9.16); defaults to the
# environment variable of the same name. Empty or invalid: no bridge, DLSS is unavailable in gow2.
# GOW2_DLSS_BRIDGE: TRUE if the bridge is built.
set(GOW2_DLSS_SDK_DIR "$ENV{GOW2_DLSS_SDK_DIR}" CACHE PATH "Checkout of the NVIDIA DLSS SDK (v310.9.16); empty = no DLSS")

set(GOW2_DLSS_BRIDGE FALSE)
set(_gow2_dlss_dll "${GOW2_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
if(GOW2_DLSS_SDK_DIR AND EXISTS "${GOW2_DLSS_SDK_DIR}/include/nvsdk_ngx.h"
   AND EXISTS "${GOW2_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" AND EXISTS "${_gow2_dlss_dll}")
    set(DLSS_SDK_ROOT "${GOW2_DLSS_SDK_DIR}")
    add_subdirectory("${CMAKE_SOURCE_DIR}/runtime/gpu/dlss_bridge" "${CMAKE_BINARY_DIR}/dlss_bridge")
    set(GOW2_DLSS_BRIDGE TRUE)
    message(STATUS "DLSS: gow2_dlss bridge built with the SDK in ${GOW2_DLSS_SDK_DIR}")
elseif(GOW2_DLSS_SDK_DIR)
    message(WARNING "DLSS: ${GOW2_DLSS_SDK_DIR} is not a checkout of the NVIDIA DLSS SDK; bridge not built")
else()
    message(STATUS "DLSS: GOW2_DLSS_SDK_DIR is not set, bridge not built")
endif()

# gow2_copy_dlss(<target> [<dir>]): gow2_dlss.dll, nvngx_dlss.dll, nvngx_dlssg.dll (rel, never dev) and licenses next
# to the executable, or in the given directory.
function(gow2_copy_dlss target)
    if(NOT GOW2_DLSS_BRIDGE)
        return()
    endif()
    set(_dir "$<TARGET_FILE_DIR:${target}>")
    if(ARGC GREATER 1)
        set(_dir "${ARGV1}")
    endif()
    add_dependencies(${target} gow2_dlss)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:gow2_dlss>" "${_gow2_dlss_dll}"
                "${GOW2_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlssg.dll" "${_dir}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/runtime/gpu/dlss_bridge/LICENSE.txt"
                "${_dir}/licenses/gow2_dlss-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GOW2_DLSS_SDK_DIR}/LICENSE.txt"
                "${_dir}/licenses/NVIDIA-RTX-SDKs-LICENSE.txt"
        VERBATIM)
endfunction()
