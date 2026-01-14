add_library(system_regex SHARED
    system_regex.cpp
    regex_functions.cpp
)

target_include_directories(system_regex PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system_regex PRIVATE qz-core)

set_target_properties(system_regex PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extensions/system_regex"
)
