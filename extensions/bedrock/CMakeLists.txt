file(GLOB SOURCES *.cpp)
file(GLOB_RECURSE HEADERS *.h *.hpp)

# Set output directory for this extension
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extensions/bedrock")

add_library(bedrock SHARED ${SOURCES})
target_include_directories(bedrock PRIVATE ../../include/core ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bedrock PRIVATE qz-core)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(bedrock PRIVATE ws2_32)
endif()

# Compiler flags for performance
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bedrock PRIVATE -O3 -march=native)
endif()
