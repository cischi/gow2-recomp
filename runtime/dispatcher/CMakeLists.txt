# gears.exe: picks a game and starts its exe (which opens its own launcher). Links no games/<id> library.
add_library(gow2_dispatcher_model STATIC dispatcher_model.cpp)
target_include_directories(gow2_dispatcher_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(gears dispatcher.cpp)
target_link_libraries(gears PRIVATE gow2_dispatcher_model gow2_imgui_host gow2_runtime)
target_compile_definitions(gears PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(gears PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
# d3d11 (and its dxgi) from System32 only: a dxgi.dll proxy (OptiScaler) next to the exe is for the games (wmain).
target_link_options(gears PRIVATE /DELAYLOAD:d3d11.dll)
target_link_libraries(gears PRIVATE delayimp)
set_target_properties(gears PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
