# FidelityFX SDK v2.3.0 (thirdparty/fidelityfx/VENDORED.md): vendored MIT headers
# (target gow2_fidelityfx) and AMD-signed DLLs downloaded here, never committed.
#
# GOW2_FIDELITYFX_DLLS: loader and upscaler, verified (SHA-256), empty if unavailable
# (download failed: warning, no error; gow2 runs with FSR unavailable).
# GOW2_FIDELITYFX_FG_DLLS: the frame generation DLL, verified separately (empty if
# unavailable or if the loader is missing); its absence does not remove FSR.
# GOW2_FIDELITYFX_DIR: local directory with the DLLs instead of downloading.

add_library(gow2_fidelityfx INTERFACE)
target_include_directories(gow2_fidelityfx INTERFACE
    "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX/api/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX/upscalers/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX/framegeneration/include")

set(GOW2_FIDELITYFX_DIR "" CACHE PATH "Directory with the FidelityFX SDK v2.3.0 DLLs (empty = download)")
set(_gow2_ffx_url "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin")
set(_gow2_ffx_files
    amd_fidelityfx_loader_dx12.dll e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa
    amd_fidelityfx_upscaler_dx12.dll d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46)
set(_gow2_ffx_fg_files
    amd_fidelityfx_framegeneration_dx12.dll 02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18)
set(_gow2_ffx_dir "${CMAKE_BINARY_DIR}/_deps/fidelityfx")

file(MAKE_DIRECTORY "${_gow2_ffx_dir}")

# Downloads/copies and verifies the DLLs in <files> (name/sha pairs); <out_var> = the verified ones, empty
# if even one is missing. <what> appears in the warning.
function(_gow2_ffx_fetch files out_var what)
    set(_dlls "")
    set(_ok TRUE)
    while(files)
        list(POP_FRONT files _name _sha)
        set(_dst "${_gow2_ffx_dir}/${_name}")
        if(GOW2_FIDELITYFX_DIR AND EXISTS "${GOW2_FIDELITYFX_DIR}/${_name}")
            file(COPY_FILE "${GOW2_FIDELITYFX_DIR}/${_name}" "${_dst}" ONLY_IF_DIFFERENT)
        elseif(NOT GOW2_FIDELITYFX_DIR AND NOT EXISTS "${_dst}")
            # Integrity comes from the pinned SHA-256 (checked below), not from TLS: CMake's curl
            # on Windows may lack the certificates (status 60). No EXPECTED_HASH: with a failed
            # download it would stop the configure instead of giving the warning.
            file(DOWNLOAD "${_gow2_ffx_url}/${_name}" "${_dst}" TLS_VERIFY OFF STATUS _st)
            list(GET _st 0 _code)
            if(NOT _code EQUAL 0)
                file(REMOVE "${_dst}")
            endif()
        endif()
        set(_got "")
        if(EXISTS "${_dst}")
            file(SHA256 "${_dst}" _got)
        endif()
        if(_got STREQUAL _sha)
            list(APPEND _dlls "${_dst}")
        else()
            file(REMOVE "${_dst}")  # copy in _deps, never the file in GOW2_FIDELITYFX_DIR
            set(_ok FALSE)
            message(WARNING "FidelityFX: ${_name} missing or with a different SHA-256 in ${_dst} (download or GOW2_FIDELITYFX_DIR); ${what} unavailable in gow2")
        endif()
    endwhile()
    if(NOT _ok)
        set(_dlls "")
    endif()
    set(${out_var} "${_dlls}" PARENT_SCOPE)
endfunction()

_gow2_ffx_fetch("${_gow2_ffx_files}" GOW2_FIDELITYFX_DLLS "FSR")
_gow2_ffx_fetch("${_gow2_ffx_fg_files}" GOW2_FIDELITYFX_FG_DLLS "frame generation")
if(NOT GOW2_FIDELITYFX_DLLS)
    set(GOW2_FIDELITYFX_FG_DLLS "")  # the loader is also needed by frame generation
endif()

# gow2_copy_fidelityfx(<target>): DLLs and license next to the executable after the build.
function(gow2_copy_fidelityfx target)
    if(NOT GOW2_FIDELITYFX_DLLS)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${GOW2_FIDELITYFX_DLLS} ${GOW2_FIDELITYFX_FG_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/LICENSE.md"
                "$<TARGET_FILE_DIR:${target}>/licenses/FidelityFX-SDK-LICENSE.md"
        VERBATIM)
endfunction()
