# SDL3 (zlib, submodule thirdparty/SDL @ release-3.4.18): only for non-Xbox
# pads (DualSense, DualShock 4, Switch Pro...) in runtime/platform/sdl_gamepads.cpp.
#
# Static library (SDL3::SDL3-static) with only the subsystems SDL_INIT_GAMEPAD
# needs: joystick, gamepad, events and HIDAPI (PS5/PS4/Switch drivers).
# Video, audio, render, GPU, camera, dialog, tray, haptic, power and sensors
# are off: the window and the audio are ours. The options are forced in the
# cache: the submodule is configured only from here.
#
# EXCLUDE_FROM_ALL: only what a project target links gets built.
# No tests, examples, SDL3_test library or install.
# Warnings off (-w): third-party code; the project does not use /WX, and SDL
# does not add it unless SDL_WERROR is on (off by default).

set(GOW2_SDL_DIR "${CMAKE_SOURCE_DIR}/thirdparty/SDL")
if(NOT EXISTS "${GOW2_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "thirdparty/SDL is missing: git submodule update --init thirdparty/SDL")
endif()

macro(_gow2_sdl_option name value)
    set(${name} ${value} CACHE BOOL "gow2: forced by cmake/SDL.cmake" FORCE)
endmacro()

# Library shape
_gow2_sdl_option(SDL_SHARED OFF)
_gow2_sdl_option(SDL_STATIC ON)
_gow2_sdl_option(SDL_TEST_LIBRARY OFF)
_gow2_sdl_option(SDL_TESTS OFF)
_gow2_sdl_option(SDL_EXAMPLES OFF)
_gow2_sdl_option(SDL_INSTALL OFF)
_gow2_sdl_option(SDL_UNINSTALL OFF)
_gow2_sdl_option(SDL_WERROR OFF)

# Subsystems off (GPU, render and camera depend on video anyway)
_gow2_sdl_option(SDL_AUDIO OFF)
_gow2_sdl_option(SDL_VIDEO OFF)
_gow2_sdl_option(SDL_GPU OFF)
_gow2_sdl_option(SDL_RENDER OFF)
_gow2_sdl_option(SDL_CAMERA OFF)
_gow2_sdl_option(SDL_HAPTIC OFF)
_gow2_sdl_option(SDL_POWER OFF)
_gow2_sdl_option(SDL_SENSOR OFF)
_gow2_sdl_option(SDL_DIALOG OFF)
_gow2_sdl_option(SDL_TRAY OFF)
_gow2_sdl_option(SDL_OPENVR OFF)
_gow2_sdl_option(SDL_OFFSCREEN OFF)

# Subsystems on: joystick + HIDAPI (gamepad and events are always built)
_gow2_sdl_option(SDL_JOYSTICK ON)
_gow2_sdl_option(SDL_HIDAPI ON)
_gow2_sdl_option(SDL_HIDAPI_JOYSTICK ON)
_gow2_sdl_option(SDL_HIDAPI_LIBUSB OFF)
_gow2_sdl_option(SDL_VIRTUAL_JOYSTICK OFF)

add_subdirectory("${GOW2_SDL_DIR}" "${CMAKE_BINARY_DIR}/thirdparty/SDL" EXCLUDE_FROM_ALL SYSTEM)
target_compile_options(SDL3-static PRIVATE -w)
