# FFmpeg subset of xenia-canary (has207/FFmpeg fork with canary's
# third_party/ffmpeg-xenia configuration; thirdparty/ffmpeg, LGPL-2.1-or-later; see
# thirdparty/ffmpeg/VENDORED.md): libavcodec + libavutil with canary's decoders
# (xmaframes, plus mp3/wma which nobody references), like the overlay's premake5.lua files.
#
# xenia_ffmpeg: a single static C11 library with the .c files of libavcodec/ and
# libavutil/ listed in thirdparty/ffmpeg/manifest.txt, except those that
# other .c files include (*_template.c, *_list.c): what remains are the same .c files that
# the fork's premake5.lua files compile for Windows (tools/vendor_ffmpeg.py).
# Like the fork's ffmpeg_common(): HAVE_AV_CONFIG_H, _USE_MATH_DEFINES, includes
# of the root and of compat/atomics/win32, bcrypt (random_seed.c uses
# BCryptGenRandom). Warnings off (-w): third-party code; our own code
# stays warning-free.
#
# The fork's code calls functions of disabled components inside constant
# `if (CONFIG_X)` (premake: optimize "Size" even in Debug, for MSVC).
# clang-cl eliminates these constant branches at any optimization level,
# even -O0/Debug, so the link always succeeds; only MSVC cl with /Od might
# leave unresolved external symbols.

set(GOW2_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOW2_FFMPEG_DIR}/manifest.txt")
file(STRINGS "${GOW2_FFMPEG_DIR}/manifest.txt" _ffmpeg_manifest REGEX "^lib(avcodec|avutil)/.*\\.c$")
set(_ffmpeg_sources "")
foreach(_rel IN LISTS _ffmpeg_manifest)
    string(STRIP "${_rel}" _rel)
    # Templates and tables included with #include "....c" are not translation
    # units: they are in the manifest only as includes.
    if(_rel MATCHES "_(template|list)\\.c$")
        continue()
    endif()
    list(APPEND _ffmpeg_sources "${GOW2_FFMPEG_DIR}/${_rel}")
endforeach()

# Must stay STATIC: libavcodec/file_open.c redefines the symbols of
# libavutil/file_open.c, and linking the objects directly would fail.
add_library(xenia_ffmpeg STATIC ${_ffmpeg_sources})
set_target_properties(xenia_ffmpeg PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(xenia_ffmpeg PRIVATE -w)
# -w does not silence clang's default errors: the overlay's codec_list.c, bsf_list.c and parser_list.c
# (included by the three files below) declare the lists as AVCodec*/AVBitStreamFilter*/AVCodecParser* where
# FFmpeg 8 wants FF*; same object, only the pointer type differs (MSVC treats it as a warning).
set_source_files_properties(
    "${GOW2_FFMPEG_DIR}/libavcodec/allcodecs.c" "${GOW2_FFMPEG_DIR}/libavcodec/bitstream_filters.c"
    "${GOW2_FFMPEG_DIR}/libavcodec/parsers.c"
    PROPERTIES COMPILE_OPTIONS -Wno-incompatible-pointer-types)
target_compile_definitions(xenia_ffmpeg PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
# The root is PUBLIC (users of the decoder include "libavcodec/avcodec.h");
# compat/atomics/win32 stays PRIVATE: its stdatomic.h must not shadow the
# compiler's one in the project's C++ code.
target_include_directories(xenia_ffmpeg SYSTEM PUBLIC "${GOW2_FFMPEG_DIR}")
# libavcodec/: files in subfolders (bsf/null.c) include libavcodec/ headers by name, like
# the includedirs of the overlay's premake5.lua.
target_include_directories(xenia_ffmpeg PRIVATE "${GOW2_FFMPEG_DIR}/compat/atomics/win32" "${GOW2_FFMPEG_DIR}/libavcodec")
target_link_libraries(xenia_ffmpeg PRIVATE bcrypt)
