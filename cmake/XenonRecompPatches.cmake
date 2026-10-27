# Applies the local patches in patches/xenonrecomp/*.patch to the
# thirdparty/XenonRecomp submodule at configure time, idempotently.
#
# The submodule gitlink stays pinned to the upstream commit (ddd128b): the
# changes only live as .patch files in this repository, so a clean clone gets
# them back. The submodule working tree therefore shows as "modified"
# (git status), which is expected.
#
# For each patch, in name order:
#   - it is applied if `git apply --check` succeeds;
#   - it is considered already applied if `git apply --reverse --check` succeeds;
#   - otherwise the configure fails with git's error.

find_package(Git REQUIRED)

set(_xr_dir "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp")
file(GLOB _xr_patches "${CMAKE_SOURCE_DIR}/patches/xenonrecomp/*.patch")
list(SORT _xr_patches)

if(NOT EXISTS "${_xr_dir}/.git")
    message(FATAL_ERROR
        "XenonRecomp: ${_xr_dir} is not a git checkout. "
        "Run: git submodule update --init --recursive")
endif()

foreach(_patch IN LISTS _xr_patches)
    get_filename_component(_name "${_patch}" NAME)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_patch}")

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${_patch}"
        WORKING_DIRECTORY "${_xr_dir}"
        RESULT_VARIABLE _fwd_rc
        ERROR_VARIABLE _fwd_err
        OUTPUT_QUIET)
    if(_fwd_rc EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${_xr_dir}"
            RESULT_VARIABLE _apply_rc
            ERROR_VARIABLE _apply_err)
        if(NOT _apply_rc EQUAL 0)
            message(FATAL_ERROR "XenonRecomp: applying ${_name} failed:\n${_apply_err}")
        endif()
        message(STATUS "XenonRecomp: applied ${_name}")
        continue()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
        WORKING_DIRECTORY "${_xr_dir}"
        RESULT_VARIABLE _rev_rc
        OUTPUT_QUIET ERROR_QUIET)
    if(_rev_rc EQUAL 0)
        message(STATUS "XenonRecomp: ${_name} already applied")
        continue()
    endif()

    message(FATAL_ERROR
        "XenonRecomp: ${_name} does not apply and does not appear to be already applied.\n"
        "The submodule must be at the pinned commit (ddd128b) with no other changes:\n"
        "  git -C thirdparty/XenonRecomp checkout -- . && git submodule update\n"
        "git apply error:\n${_fwd_err}")
endforeach()
