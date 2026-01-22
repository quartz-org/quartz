cmake_minimum_required(VERSION 3.10)

project(Quartz VERSION 0.1.0 LANGUAGES CXX)

# ============================================================================
# Build Options (can be set from build.sh via -D flags)
# ============================================================================
option(QZ_JIT_ENABLED "Enable Copy-and-Patch JIT compiler" ON)
option(QZ_JIT_DEBUG "Enable JIT debug output" OFF)
option(QZ_LTO_ENABLED "Enable Link-Time Optimization" ON)
option(QZ_NATIVE_ARCH "Enable native architecture optimizations" OFF)
option(QZ_VM_DEBUG_TRACE "Enable VM debug tracing" OFF)
option(QZ_VM_INSTRUCTION_CACHE "Enable VM instruction caching" ON)
option(QZ_VM_COMPUTED_GOTOS "Enable computed gotos for instruction dispatch (GCC/Clang only)" ON)

set(QZ_JIT_THRESHOLD "100" CACHE STRING "JIT compilation threshold")
set(QZ_VM_STACK_SIZE "65536" CACHE STRING "VM stack size")
set(QZ_HEAP_INITIAL_MB "64" CACHE STRING "Initial heap size in MB")
set(QZ_HEAP_MAX_MB "0" CACHE STRING "Maximum heap size in MB (0=unlimited)")
set(QZ_ARCH "" CACHE STRING "Target architecture for JIT (auto-detected if empty)")

# ============================================================================
# Compiler Configuration
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(APPLE)
    set(PLATFORM_NAME "macOS")
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_NAME "Linux")
elseif(WIN32)
    set(PLATFORM_NAME "Windows")
else()
    set(PLATFORM_NAME "Unknown")
endif()

message(STATUS "Building for platform: ${PLATFORM_NAME}")

# Symbol visibility
if(NOT WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=default")
endif()

# Performance optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Release build - enabling optimizations")
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /DNDEBUG")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
        # LTO/IPO support
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if(ipo_supported)
            message(STATUS "IPO/LTO enabled")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(STATUS "IPO/LTO not supported: ${ipo_error}")
        endif()
    endif()
else()
    message(STATUS "Debug build - moderate optimizations")
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O1")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g")
    endif()
endif()

# Platform-specific compiler flags
if(UNIX AND NOT APPLE)
    # Linux-specific flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

# ============================================================================
# Build Configuration
# ============================================================================

# Keep outputs in ./build/ (scripts and tooling assume this)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# RPATH settings for dynamic library loading
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# ============================================================================
# Include Directories
# ============================================================================
include_directories(include)
include_directories(include/core)
include_directories(jit)

# ============================================================================
# JIT Configuration
# ============================================================================
if(QZ_JIT_ENABLED)
    message(STATUS "JIT compilation: ENABLED (threshold=${QZ_JIT_THRESHOLD})")
    add_definitions(-DQZ_JIT_ENABLED=1)
    add_definitions(-DQZ_JIT_THRESHOLD=${QZ_JIT_THRESHOLD})
    
    if(QZ_JIT_DEBUG)
        add_definitions(-DQZ_JIT_DEBUG=1)
    endif()
    
    # Detect architecture for JIT stencils
    if(QZ_ARCH STREQUAL "")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set(QZ_ARCH "x86_64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(QZ_ARCH "aarch64")
        else()
            message(WARNING "Unknown architecture ${CMAKE_SYSTEM_PROCESSOR}, JIT disabled")
            set(QZ_JIT_ENABLED OFF)
        endif()
    endif()
    
    if(QZ_JIT_ENABLED)
        message(STATUS "JIT target architecture: ${QZ_ARCH}")
        add_definitions(-DQZ_JIT_ARCH_${QZ_ARCH}=1)
    endif()
else()
    message(STATUS "JIT compilation: DISABLED")
endif()

# VM Configuration
add_definitions(-DQZ_VM_STACK_SIZE=${QZ_VM_STACK_SIZE})
if(QZ_VM_INSTRUCTION_CACHE)
    add_definitions(-DQZ_VM_INSTRUCTION_CACHE=1)
endif()
if(QZ_VM_DEBUG_TRACE)
    add_definitions(-DQZ_VM_DEBUG_TRACE=1)
endif()
# Computed gotos: only supported by GCC/Clang, not MSVC
if(QZ_VM_COMPUTED_GOTOS)
    if(MSVC)
        message(STATUS "Computed gotos requested but not supported by MSVC - disabled")
        set(QZ_VM_COMPUTED_GOTOS OFF)
    else()
        message(STATUS "Computed gotos: ENABLED")
        add_definitions(-DQZ_VM_COMPUTED_GOTOS=1)
    endif()
else()
    message(STATUS "Computed gotos: DISABLED")
endif()

# Memory configuration
add_definitions(-DQZ_HEAP_INITIAL_MB=${QZ_HEAP_INITIAL_MB})
add_definitions(-DQZ_HEAP_MAX_MB=${QZ_HEAP_MAX_MB})

# ============================================================================
# Extensions
# ============================================================================
add_subdirectory(extensions)

# ============================================================================
# Core Library
# ============================================================================
file(GLOB_RECURSE CORE_SOURCES
    src/core/*.cpp
)

# Remove main.cpp from core sources (it's for the executable)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/core/main.cpp)

# Add JIT sources if enabled
if(QZ_JIT_ENABLED)
    list(APPEND CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/jit/jit.cpp)
endif()

add_library(qz-core SHARED ${CORE_SOURCES})

# Set library properties
set_target_properties(qz-core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "qz-core"
)

# ============================================================================
# Main Executable
# ============================================================================
add_executable(quartz src/core/main.cpp)

# Platform-specific linking
if(APPLE)
    # macOS: Use -force_load to ensure all symbols from core library are available
    target_link_libraries(quartz PRIVATE -Wl,-force_load qz-core)
elseif(UNIX)
    # Linux: Use --whole-archive to ensure all symbols are available
    target_link_libraries(quartz PRIVATE 
        -Wl,--whole-archive 
        qz-core 
        -Wl,--no-whole-archive
    )
    # Link with dl for dynamic loading and pthread
    target_link_libraries(quartz PRIVATE dl pthread)
else()
    # Fallback for other platforms
    target_link_libraries(quartz PRIVATE qz-core)
endif()

# Set executable properties
set_target_properties(quartz PROPERTIES
    OUTPUT_NAME "quartz"
)

# ============================================================================
# Installation (optional)
# ============================================================================
install(TARGETS quartz qz-core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "==============================================")
message(STATUS "Quartz Language Build Configuration")
message(STATUS "==============================================")
message(STATUS "Platform:       ${PLATFORM_NAME}")
message(STATUS "Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "----------------------------------------------")
message(STATUS "JIT enabled:    ${QZ_JIT_ENABLED}")
if(QZ_JIT_ENABLED)
message(STATUS "JIT threshold:  ${QZ_JIT_THRESHOLD}")
message(STATUS "JIT arch:       ${QZ_ARCH}")
message(STATUS "JIT debug:      ${QZ_JIT_DEBUG}")
endif()
message(STATUS "LTO enabled:    ${QZ_LTO_ENABLED}")
message(STATUS "Native arch:    ${QZ_NATIVE_ARCH}")
message(STATUS "VM stack size:  ${QZ_VM_STACK_SIZE}")
message(STATUS "Computed gotos: ${QZ_VM_COMPUTED_GOTOS}")
message(STATUS "==============================================")