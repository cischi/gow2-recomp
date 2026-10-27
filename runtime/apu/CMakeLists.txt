# gow2_apu: audio (XAudio driver client, Audio Worker, WAV capture, XAudio*
# exports, XMA decoder using xenia-canary's XmaContextNew/Old, gow2::hooks
# glue from patch 0007 (thirdparty/xenia/patches) into canary's APU). Uses only
# the standard library, plat::, the HLE kernel and xenia_apu.
#
# tools/gen_import_stubs.py also reads KERNEL_EXPORT from the .cpp files in
# this folder (cmake/Recompile.cmake): every .cpp must be listed here.
add_library(gow2_apu STATIC
    audio_exports.cpp
    audio_system.cpp
    wav_writer.cpp
    xma_decoder.cpp
    xma_hooks.cpp)
target_include_directories(gow2_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# xenia_apu (XmaContext) uses the hooks in xma_hooks.cpp: the two libraries
# call each other (lld-link resolves static libraries in any order).
target_link_libraries(gow2_apu PUBLIC xenia_apu gow2_kernel gow2_runtime)
