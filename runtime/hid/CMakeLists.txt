# gow2_hid: input (InputSystem with ordered drivers, XamInput* exports,
# script/XInput/keyboard drivers, --input-script). Uses only the standard
# library, plat:: and the HLE kernel.
#
# tools/gen_import_stubs.py also reads KERNEL_EXPORT from the .cpp files in
# this folder (cmake/Recompile.cmake): every .cpp must be listed here.
add_library(gow2_hid STATIC
    drivers.cpp
    input_exports.cpp
    input_script.cpp
    input_system.cpp
    sdl_driver.cpp)
target_include_directories(gow2_hid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gow2_hid PUBLIC gow2_kernel gow2_runtime)
