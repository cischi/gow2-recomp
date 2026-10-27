# The Windows SDK's fxc and Xenia's D3D12 shaders, compiled by the build.
#
# GOW2_FXC is also needed by the replacement shaders (runtime/gpu/CMakeLists.txt).
# The WindowsSdk* variables are set by vcvars; without vcvars, fxc sits next to
# rc.exe of the same SDK.
#
# Xenia's bytecode headers (xenia/<gpu|ui>/shaders/bytecode/d3d12_5_1/
# <id>.h) are not vendored: tools/vendor_xenia.py copies the sources
# <base>.<stage>.xesl/.hlsl and here fxc compiles them into
# ${CMAKE_BINARY_DIR}/xenia_shaders, as xenia-build does (id = name without
# extension with '.' -> '_', stage = last two letters of the id).

get_filename_component(_gow2_rc_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
set(_gow2_fxc_hints "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/$ENV{WindowsSDKVersion}/x64"
    "${_gow2_rc_dir}")
find_program(GOW2_FXC fxc HINTS ${_gow2_fxc_hints})
if(NOT GOW2_FXC)
    message(FATAL_ERROR "Windows SDK fxc not found (searched in: ${_gow2_fxc_hints} and on PATH)")
endif()

# fxc flags for Xenia's shaders, like tools/build/compile_shader_dxbc.py
# of xenia-canary; /T, /Vn, /Fh and /I are added by gow2_xenia_dxbc_shaders.
set(GOW2_XENIA_FXC_FLAGS /D SHADING_LANGUAGE_HLSL_XE=1 /O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv
    /Gfp /nologo)
set(GOW2_XENIA_SHADERS_DIR "${CMAKE_BINARY_DIR}/xenia_shaders")

# gow2_xenia_dxbc_shaders(<target> <shaders dir> <xenia/gpu|xenia/ui>):
# compiles every *.xesl/*.hlsl that has a stage in the directory into
# ${GOW2_XENIA_SHADERS_DIR}/<subdir>/shaders/bytecode/d3d12_5_1/<id>.h,
# adds the headers to <target>'s sources (generated before its
# objects) and ${GOW2_XENIA_SHADERS_DIR} to its PUBLIC includes. A shader that
# does not compile stops the build on its own command (COMMENT carries the file name).
function(gow2_xenia_dxbc_shaders target shaders_dir subdir)
    file(GLOB _sources CONFIGURE_DEPENDS "${shaders_dir}/*.xesl" "${shaders_dir}/*.hlsl")
    # Dependencies: the includes of all shader folders and of the vendored
    # third_party (enough to recompile when they change).
    file(GLOB_RECURSE _includes CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/thirdparty/xenia/src/xenia/*.xesli"
        "${CMAKE_SOURCE_DIR}/thirdparty/xenia/src/xenia/*.hlsli"
        "${CMAKE_SOURCE_DIR}/thirdparty/xenia/third_party/*.h")
    set(_out_dir "${GOW2_XENIA_SHADERS_DIR}/${subdir}/shaders/bytecode/d3d12_5_1")
    set(_outputs "")
    foreach(_src IN LISTS _sources)
        get_filename_component(_name "${_src}" NAME)
        string(REGEX REPLACE "\\.(xesl|hlsl)$" "" _id "${_name}")
        string(REPLACE "." "_" _id "${_id}")
        string(REGEX MATCH "_(vs|hs|ds|gs|ps|cs)$" _stage_suffix "${_id}")
        if(NOT _stage_suffix)
            message(FATAL_ERROR "${_src}: Xenia shader without a stage (vs hs ds gs ps cs)")
        endif()
        string(SUBSTRING "${_stage_suffix}" 1 2 _stage)
        set(_out "${_out_dir}/${_id}.h")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
            COMMAND "${GOW2_FXC}" ${GOW2_XENIA_FXC_FLAGS} /T ${_stage}_5_1 /Vn ${_id}
                    /Fh "${_out}" /I "${shaders_dir}" "${_src}"
            DEPENDS "${_src}" ${_includes}
            COMMENT "fxc ${subdir}/shaders/${_name}"
            VERBATIM)
        list(APPEND _outputs "${_out}")
    endforeach()
    target_sources(${target} PRIVATE ${_outputs})
    target_include_directories(${target} PUBLIC "${GOW2_XENIA_SHADERS_DIR}")
endfunction()
