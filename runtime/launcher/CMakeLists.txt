# gow2_launcher: settings screen shown before startup. Model without
# ImGui, ImGui panel, Win32 + D3D11 window.
add_library(gow2_launcher STATIC
    game_setup.cpp
    launcher_model.cpp
    launcher_text.cpp
    launcher_ui.cpp
    launcher.cpp
    nvidia_dlls.cpp
    optiscaler.cpp)
# Window plumbing (D3D11 swap chain, placement) and ImGui style: no gow2_runtime and no game library, shared
# with gears.exe.
add_library(gow2_imgui_host STATIC imgui_style.cpp win_d3d11.cpp)
target_include_directories(gow2_imgui_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gow2_imgui_host PUBLIC gow2_imgui)
target_compile_definitions(gow2_imgui_host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_include_directories(gow2_launcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gow2_launcher PUBLIC gow2_runtime gow2_imgui_host gow2_imgui gow2_extract_lib)
# launcher_model.cpp calls game_patches::Table() and game_profile::Get(): the consumer links a game library
# (gow2_game, or a test fixture that defines both), not linked here so tests can swap it.
# OptiScaler: download (WinHTTP), SHA-256 (BCrypt), dxgi.dll version block.
target_link_libraries(gow2_launcher PRIVATE winhttp bcrypt version comdlg32)
target_compile_definitions(gow2_launcher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
