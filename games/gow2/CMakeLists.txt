# gow2_game: Gears of War 2 profile. Expected SHA-256 of the decrypted default.xex image from expected_xex.json.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/expected_xex.json" _gow2_expected_json)
string(JSON _gow2_xex_sha256 GET "${_gow2_expected_json}" image_sha256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/expected_xex.json")
add_library(gow2_game STATIC profile.cpp patches.cpp)
target_include_directories(gow2_game PUBLIC ${CMAKE_SOURCE_DIR}/runtime)
target_compile_definitions(gow2_game PRIVATE GAME_XEX_SHA256="${_gow2_xex_sha256}")
target_link_libraries(gow2_game PRIVATE XenonUtils)  # ppc_context.h for the Gow2Patch* hooks
