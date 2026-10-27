# Dear ImGui (MIT, submodule thirdparty/imgui @ v1.93.0): core and the
# win32 + dx11 (launcher, runtime/launcher) and dx12 (F1 overlay, runtime/gpu/overlay.cpp) backends. Warnings off (-w):
# third-party code.
set(GOW2_IMGUI_DIR "${CMAKE_SOURCE_DIR}/thirdparty/imgui")
if(NOT EXISTS "${GOW2_IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "thirdparty/imgui is missing: git submodule update --init thirdparty/imgui")
endif()
add_library(gow2_imgui STATIC
    "${GOW2_IMGUI_DIR}/imgui.cpp"
    "${GOW2_IMGUI_DIR}/imgui_draw.cpp"
    "${GOW2_IMGUI_DIR}/imgui_tables.cpp"
    "${GOW2_IMGUI_DIR}/imgui_widgets.cpp"
    "${GOW2_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${GOW2_IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${GOW2_IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
target_include_directories(gow2_imgui SYSTEM PUBLIC "${GOW2_IMGUI_DIR}" "${GOW2_IMGUI_DIR}/backends")
# No deprecated functions: the launcher code only uses the current API.
target_compile_definitions(gow2_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(gow2_imgui PRIVATE -w)
target_link_libraries(gow2_imgui PUBLIC d3d11 d3d12 dxgi d3dcompiler)
