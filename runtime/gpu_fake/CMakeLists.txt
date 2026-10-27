# gow2_gpu_fake: fake GPU backend: command
# processor thread on the ring buffer and a mini PM4 interpreter, no renderer.
# Registers, vblank and Vd* live in the common module (gow2_gpu). Builds
# without the generated code, like gow2_kernel.
add_library(gow2_gpu_fake STATIC
    gpu.cpp
    pm4.cpp)
# Includes are "gpu_fake/xxx.h", relative to the runtime/ folder.
target_include_directories(gow2_gpu_fake PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(gow2_gpu_fake PUBLIC gow2_gpu gow2_kernel PRIVATE gow2_runtime)
