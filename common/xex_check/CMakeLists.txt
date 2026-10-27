# Structural checks of an XEX2 before Image::ParseImage (which does none).
# Shared by xexdump and the runtime (LoadImage), so the rule about the bytes
# actually produced by the XenonUtils loader lives in a single place.
add_library(gow2_xex_check STATIC xex_check.cpp)
target_include_directories(gow2_xex_check PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Only for the XEX constants and structs (<xex.h>); the public header does not expose them.
target_link_libraries(gow2_xex_check PRIVATE XenonUtils)
target_compile_definitions(gow2_xex_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
