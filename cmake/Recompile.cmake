# Static recompilation of default.xex with XenonRecomp.
#
# Included by CMakeLists.txt: defines one recompile target per game (gow2_add_recompile;
# outside `all`, because it needs the user's game data) and the variables
# <ID>_GENERATED_SOURCES with the glob of its generated *.cpp.
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
            "Recompile: ${XEX} not found. Extract the disc into its game data folder first "
            "(python tools/gow2.py build, see tools/extract).")
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

if(Python3_Interpreter_FOUND)
    set(_gow2_python "${Python3_EXECUTABLE}")
else()
    set(_gow2_python "Python3-NOTFOUND")
endif()

# gow2_add_recompile(<id> DATA_DIR <dir>): target that recompiles <dir>/default.xex with
# games/<id>/<id>.toml (`recompile` for gow2, `recompile_<id>` for the others) and, in the
# caller's scope, <ID>_GENERATED_DIR (build/generated for gow2, build/generated_<id> for the
# others: it must match out_directory_path in the TOML), <ID>_RECOMP_LOG, <ID>_GENERATED_SOURCES,
# <ID>_HAVE_GENERATED and <ID>_IMPORT_STUBS (with the <id>_import_stubs target).
function(gow2_add_recompile id)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "DATA_DIR" "")
    string(TOUPPER "${id}" _ID)
    set(_toml "${CMAKE_SOURCE_DIR}/games/${id}/${id}.toml")
    if(id STREQUAL "gow2")
        set(_target recompile)
        set(_generated "${CMAKE_SOURCE_DIR}/build/generated")
        set(_log "${CMAKE_SOURCE_DIR}/build/recomp.log")
        set(_stubs "${CMAKE_BINARY_DIR}/import_stubs/import_stubs.cpp")
    else()
        set(_target recompile_${id})
        set(_generated "${CMAKE_SOURCE_DIR}/build/generated_${id}")
        set(_log "${CMAKE_SOURCE_DIR}/build/recomp_${id}.log")
        set(_stubs "${CMAKE_BINARY_DIR}/import_stubs_${id}/import_stubs.cpp")
    endif()

    gow2_check_recomp_options("${_toml}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_toml}")

    add_custom_target(${_target}
        COMMAND "${CMAKE_COMMAND}"
            "-DXENON_RECOMP=$<TARGET_FILE:XenonRecomp>"
            "-DCONFIG=${_toml}"
            "-DPPC_CONTEXT=${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/XenonUtils/ppc_context.h"
            "-DGENERATED_DIR=${_generated}"
            "-DLOG=${_log}"
            "-DPYTHON=${_gow2_python}"
            "-DCHECKER=${CMAKE_SOURCE_DIR}/tools/check_recomp_log.py"
            "-DXEX=${CMAKE_SOURCE_DIR}/${_arg_DATA_DIR}/default.xex"
            -P "${CMAKE_SOURCE_DIR}/cmake/Recompile.cmake"
        DEPENDS XenonRecomp
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Recompiling ${_arg_DATA_DIR}/default.xex with XenonRecomp"
        USES_TERMINAL
        VERBATIM)

    # Generated sources, for the game target (runtime/CMakeLists.txt). Empty
    # until the recompile target has been run; CONFIGURE_DEPENDS makes the glob repeat.
    file(GLOB _sources CONFIGURE_DEPENDS "${_generated}/*.cpp")

    # Stubs for the imports (__imp__Name) declared in ppc_recomp_shared.h and not
    # implemented in runtime/kernel/, runtime/gpu/, runtime/gpu_fake/, runtime/apu/ or
    # runtime/hid/ (KERNEL_EXPORT).
    # Regenerated on every new recompile run (they depend on the header) and
    # written to the build folder, never to the generated folder (which the recompile
    # target cleans) nor to the repo. They only exist if the generated code is there.
    if(EXISTS "${_generated}/ppc_func_mapping.cpp" AND EXISTS "${_generated}/ppc_recomp_shared.h")
        set(_have ON)
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR "Python 3 not found: needed for tools/gen_import_stubs.py")
        endif()
        # Imports implemented with KERNEL_EXPORT in the .cpp files of runtime/kernel/,
        # runtime/gpu/ (Vd*), runtime/gpu_fake/, runtime/apu/ (XAudio*) and runtime/hid/
        # (XamInput*) get no stub: regenerated when those sources change
        # (CONFIGURE_DEPENDS repeats the glob if any are added). A folder that
        # does not exist is not passed to gen_import_stubs.py, which would
        # reject it; the glob rechecks it.
        set(_export_dirs kernel gpu gpu_fake apu hid)
        set(_kernel_globs)
        set(_implemented_from)
        foreach(_dir IN LISTS _export_dirs)
            list(APPEND _kernel_globs "${CMAKE_SOURCE_DIR}/runtime/${_dir}/*.cpp")
            if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/runtime/${_dir}")
                list(APPEND _implemented_from --implemented-from "${CMAKE_SOURCE_DIR}/runtime/${_dir}")
            endif()
        endforeach()
        file(GLOB_RECURSE _kernel_sources CONFIGURE_DEPENDS ${_kernel_globs})
        add_custom_command(
            OUTPUT "${_stubs}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
                "${_generated}/ppc_recomp_shared.h" "${_stubs}"
                ${_implemented_from}
            DEPENDS "${_generated}/ppc_recomp_shared.h" "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
                ${_kernel_sources}
            COMMENT "Import stubs from ${_generated}/ppc_recomp_shared.h"
            VERBATIM)
        add_custom_target(${id}_import_stubs DEPENDS "${_stubs}")
    else()
        set(_have OFF)
    endif()

    set(${_ID}_GENERATED_DIR "${_generated}" PARENT_SCOPE)
    set(${_ID}_RECOMP_LOG "${_log}" PARENT_SCOPE)
    set(${_ID}_GENERATED_SOURCES "${_sources}" PARENT_SCOPE)
    set(${_ID}_HAVE_GENERATED ${_have} PARENT_SCOPE)
    set(${_ID}_IMPORT_STUBS "${_stubs}" PARENT_SCOPE)
endfunction()

gow2_add_recompile(gow2 DATA_DIR game_data)
gow2_add_recompile(gow3 DATA_DIR game_data_gow3)
