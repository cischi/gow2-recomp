add_library(gow2_xdvdfs STATIC xdvdfs.cpp)
target_include_directories(gow2_xdvdfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(gow2_extract_lib STATIC run_extract.cpp)
target_link_libraries(gow2_extract_lib PUBLIC gow2_xdvdfs)

add_executable(extract main.cpp)
target_link_libraries(extract PRIVATE gow2_extract_lib)
