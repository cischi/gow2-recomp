# Static recompilation of default.xex with XenonRecomp.
#
# Included by CMakeLists.txt: defines the `recompile` target (outside `all`,
# because it needs the user's game data) and the variable
# GOW2_GENERATED_SOURCES with the glob of build/generated/*.cpp.
#
# Run with `cmake -P` (from the target): launches XenonRecomp, saves the output in
# build/recomp.log and fails if check_recomp_log.py finds errors.

# gow2_check_recomp_options(<toml>): gow2_kernel, gow2_gpu and gow2_gpu_fake compile
# ppc_context.h without the generated ppc_config.h (runtime/kernel/ppc_layout.h):
# an enabled skip_* / *_as_local option would change the layout of PPCContext
# in the generated code only. Until the kernel supports them, they are rejected.
function(gow2_check_recomp_options toml)
    if(NOT EXISTS "${toml}")
        return()
    endif()
    file(STRINGS "${toml}" _lines REGEX "^[ \t]*(skip_[A-Za-z0-9_]+|[A-Za-z0-9_]+_as_local)[ \t]*=[ \t]*true")
    if(_lines)
        list(JOIN _lines "\n  " _shown)
        message(FATAL_ERROR
            "${toml}: options not supported by the HLE kernel (they change the layout of PPCContext "
            "relative to gow2_kernel, see runtime/kernel/ppc_layout.h):\n  ${_shown}\n"
            "Leave skip_* and *_as_local set to false.")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    # ---- script mode: variables passed with -D by the target ----
    foreach(var XENON_RECOMP CONFIG PPC_CONTEXT GENERATED_DIR LOG PYTHON CHECKER XEX)
        if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
            message(FATAL_ERROR "Recompile.cmake: variable ${var} is not defined")
        endif()
    endforeach()

    if(NOT PYTHON OR NOT EXISTS "${PYTHON}")
        message(FATAL_ERROR "Recompile: Python 3 interpreter not found at configure time "
            "(needed for tools/check_recomp_log.py)")
    endif()
    if(NOT EXISTS "${XEX}")
        message(FATAL_ERROR
            "Recompile: ${XEX} not found. Extract the disc into game_data/ first "
            "(see tools/extract).")
    endif()
    # If the TOML is not a regular file, XenonRecomp silently runs its internal
    # tests and exits with 0: this case must be excluded explicitly.
    if(NOT EXISTS "${CONFIG}" OR IS_DIRECTORY "${CONFIG}")
        message(FATAL_ERROR "Recompile: configuration ${CONFIG} not found")
    endif()
    gow2_check_recomp_options("${CONFIG}")

    # XenonRecomp does not create the output folder and does not check the result of fopen.
    file(MAKE_DIRECTORY "${GENERATED_DIR}")
    # Functions are spread 256 per file (ppc_recomp.N.cpp): if a config fix
    # reduces the number of functions, the high-N files of a previous run
    # would remain and duplicate symbols.
    # Deleting everything BEFORE the run would force recompiling ~231 MiB
    # every time (XenonRecomp does not rewrite identical files, but here they would not exist).
    # So the previous output is moved to a .prev folder, XenonRecomp is let
    # generate into an empty folder and, once the run succeeds, every file
    # identical to the previous one is put back in place (same mtime: ninja
    # does not recompile). What remains in .prev (files no longer produced) is stale
    # and is discarded. More robust than deducing N from the number of functions.
    set(_prev "${GENERATED_DIR}.prev")
    if(EXISTS "${_prev}")
        # interrupted run: the oldest output is still the reference
        file(REMOVE_RECURSE "${GENERATED_DIR}")
    else()
        file(RENAME "${GENERATED_DIR}" "${_prev}")
    endif()
    file(MAKE_DIRECTORY "${GENERATED_DIR}")

    message(STATUS "XenonRecomp ${CONFIG} -> ${GENERATED_DIR} (log: ${LOG})")
    execute_process(
        COMMAND "${XENON_RECOMP}" "${CONFIG}" "${PPC_CONTEXT}"
        OUTPUT_FILE "${LOG}"
        ERROR_FILE "${LOG}"
        RESULT_VARIABLE rc)
    if(NOT rc STREQUAL "0")
        message(FATAL_ERROR "Recompile: XenonRecomp exited with code ${rc} (see ${LOG})")
    endif()
    # Put the unchanged files back (original mtime), discard the rest.
    file(GLOB _new_files RELATIVE "${GENERATED_DIR}" "${GENERATED_DIR}/*")
    set(_kept 0)
    foreach(_f IN LISTS _new_files)
        if(EXISTS "${_prev}/${_f}")
            execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${GENERATED_DIR}/${_f}" "${_prev}/${_f}"
                RESULT_VARIABLE _same OUTPUT_QUIET ERROR_QUIET)
            if(_same EQUAL 0)
                file(RENAME "${_prev}/${_f}" "${GENERATED_DIR}/${_f}")
                math(EXPR _kept "${_kept}+1")
            endif()
        endif()
    endforeach()
    file(GLOB _stale RELATIVE "${_prev}" "${_prev}/*")
    list(LENGTH _stale _stale_n)
    list(LENGTH _new_files _new_n)
    file(REMOVE_RECURSE "${_prev}")
    message(STATUS "Recompile: ${_new_n} files, ${_kept} unchanged, ${_stale_n} stale/modified discarded")

    execute_process(
        COMMAND "${PYTHON}" "${CHECKER}" "${LOG}" --generated "${GENERATED_DIR}"
        RESULT_VARIABLE rc)
    if(NOT rc STREQUAL "0")
        message(FATAL_ERROR "Recompile: the XenonRecomp log contains errors (see ${LOG})")
    endif()

    if(NOT EXISTS "${GENERATED_DIR}/ppc_func_mapping.cpp")
        message(FATAL_ERROR "Recompile: ${GENERATED_DIR}/ppc_func_mapping.cpp was not generated")
    endif()
    return()
endif()

# ---- configure mode ----
find_package(Python3 COMPONENTS Interpreter)

gow2_check_recomp_options("${CMAKE_SOURCE_DIR}/config/gow2.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/config/gow2.toml")

set(GOW2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated")
set(GOW2_RECOMP_LOG "${CMAKE_SOURCE_DIR}/build/recomp.log")

if(Python3_Interpreter_FOUND)
    set(_gow2_python "${Python3_EXECUTABLE}")
else()
    set(_gow2_python "Python3-NOTFOUND")
endif()

add_custom_target(recompile
    COMMAND "${CMAKE_COMMAND}"
        "-DXENON_RECOMP=$<TARGET_FILE:XenonRecomp>"
        "-DCONFIG=${CMAKE_SOURCE_DIR}/config/gow2.toml"
        "-DPPC_CONTEXT=${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/XenonUtils/ppc_context.h"
        "-DGENERATED_DIR=${GOW2_GENERATED_DIR}"
        "-DLOG=${GOW2_RECOMP_LOG}"
        "-DPYTHON=${_gow2_python}"
        "-DCHECKER=${CMAKE_SOURCE_DIR}/tools/check_recomp_log.py"
        "-DXEX=${CMAKE_SOURCE_DIR}/game_data/default.xex"
        -P "${CMAKE_CURRENT_LIST_DIR}/Recompile.cmake"
    DEPENDS XenonRecomp
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Recompiling default.xex with XenonRecomp"
    USES_TERMINAL
    VERBATIM)

# Generated sources, for the game target (runtime/CMakeLists.txt). Empty
# until `recompile` has been run; CONFIGURE_DEPENDS makes the glob repeat.
file(GLOB GOW2_GENERATED_SOURCES CONFIGURE_DEPENDS "${GOW2_GENERATED_DIR}/*.cpp")

# Stubs for the imports (__imp__Name) declared in ppc_recomp_shared.h and not
# implemented in runtime/kernel/, runtime/gpu/, runtime/gpu_fake/, runtime/apu/ or
# runtime/hid/ (KERNEL_EXPORT).
# Regenerated on every new `recompile` run (they depend on the header) and
# written to the build folder, never to build/generated (which `recompile`
# cleans) nor to the repo. They only exist if the generated code is there.
set(GOW2_IMPORT_STUBS "${CMAKE_BINARY_DIR}/import_stubs/import_stubs.cpp")
if(EXISTS "${GOW2_GENERATED_DIR}/ppc_func_mapping.cpp" AND EXISTS "${GOW2_GENERATED_DIR}/ppc_recomp_shared.h")
    set(GOW2_HAVE_GENERATED ON)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "Python 3 not found: needed for tools/gen_import_stubs.py")
    endif()
    # Imports implemented with KERNEL_EXPORT in the .cpp files of runtime/kernel/,
    # runtime/gpu/ (Vd*), runtime/gpu_fake/, runtime/apu/ (XAudio*) and runtime/hid/
    # (XamInput*) get no stub: regenerated when those sources change
    # (CONFIGURE_DEPENDS repeats the glob if any are added). A folder that
    # does not exist is not passed to gen_import_stubs.py, which would
    # reject it; the glob rechecks it.
    set(_gow2_export_dirs kernel gpu gpu_fake apu hid)
    set(_gow2_kernel_globs)
    set(_gow2_implemented_from)
    foreach(_dir IN LISTS _gow2_export_dirs)
        list(APPEND _gow2_kernel_globs "${CMAKE_SOURCE_DIR}/runtime/${_dir}/*.cpp")
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/runtime/${_dir}")
            list(APPEND _gow2_implemented_from --implemented-from "${CMAKE_SOURCE_DIR}/runtime/${_dir}")
        endif()
    endforeach()
    file(GLOB_RECURSE _gow2_kernel_sources CONFIGURE_DEPENDS ${_gow2_kernel_globs})
    add_custom_command(
        OUTPUT "${GOW2_IMPORT_STUBS}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
            "${GOW2_GENERATED_DIR}/ppc_recomp_shared.h" "${GOW2_IMPORT_STUBS}"
            ${_gow2_implemented_from}
        DEPENDS "${GOW2_GENERATED_DIR}/ppc_recomp_shared.h" "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
            ${_gow2_kernel_sources}
        COMMENT "Import stubs from ppc_recomp_shared.h"
        VERBATIM)
    add_custom_target(gow2_import_stubs DEPENDS "${GOW2_IMPORT_STUBS}")
else()
    set(GOW2_HAVE_GENERATED OFF)
endif()
