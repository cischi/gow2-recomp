# gow2_launcher: settings screen shown before startup. Model without
# ImGui, ImGui panel, Win32 + D3D11 window.
add_library(gow2_launcher STATIC
    game_setup.cpp
    imgui_style.cpp
    launcher_model.cpp
    launcher_text.cpp
    launcher_ui.cpp
    launcher.cpp
    nvidia_dlls.cpp
    optiscaler.cpp)
target_include_directories(gow2_launcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gow2_launcher PUBLIC gow2_runtime gow2_imgui gow2_extract_lib)
# OptiScaler: download (WinHTTP), SHA-256 (BCrypt), dxgi.dll version block.
target_link_libraries(gow2_launcher PRIVATE winhttp bcrypt version comdlg32)
target_compile_definitions(gow2_launcher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
