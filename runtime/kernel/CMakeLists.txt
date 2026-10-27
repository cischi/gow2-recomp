# gow2_kernel: HLE Xbox 360 kernel (KERNEL_EXPORT implementations of the
# imports, argument marshalling, summary counters). Builds without the
# generated code: it uses XenonUtils' ppc_context.h, with the same layout as the
# PPCContext of the one copied into build/generated (no PPC_CONFIG_*
# option enabled in config/gow2.toml).
#
# tools/gen_import_stubs.py reads the KERNEL_EXPORTs from the .cpp files in this folder
# (cmake/Recompile.cmake) and does not generate a stub for those names.

# o1heap (MIT, thirdparty/o1heap @ 3.0.0): O(1) allocator for the guest heaps.
add_library(o1heap STATIC "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap/o1heap.c")
target_include_directories(o1heap PUBLIC "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap")
# o1heap.c uses __builtin_clzl with clang: on Windows long is 32 bit and the log2
# of size_t would be truncated (wrong bins, heap corruption). 64-bit variant.
target_compile_definitions(o1heap PRIVATE O1HEAP_CLZ=__builtin_clzll)

add_library(gow2_kernel STATIC
    io/file_exports.cpp
    io/vfs.cpp
    memory.cpp
    module.cpp
    objects.cpp
    rtl_exports.cpp
    stats.cpp
    stubs_exports.cpp
    sync.cpp
    sync_exports.cpp
    thread_exports.cpp
    threads.cpp
    xam_exports.cpp
    xma.cpp)
# Includes are "kernel/xxx.h", from the runtime/ folder.
target_include_directories(gow2_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# XenonUtils: be<T> (xbox.h, which includes windows.h) and ppc_context.h/simde.
target_link_libraries(gow2_kernel PUBLIC XenonUtils PRIVATE gow2_runtime o1heap)
target_compile_definitions(gow2_kernel PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
