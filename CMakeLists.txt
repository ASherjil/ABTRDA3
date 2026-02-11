# =============================================================================
# ABTRDA3 — Ultra-Low Latency Packet MMAP Library
# =============================================================================

cmake_minimum_required(VERSION 3.21)

project(ABTRDA3
        VERSION 0.1.0
        DESCRIPTION "packet_mmap TX/RX for WREN timing sidecar"
        LANGUAGES CXX
)

# =============================================================================
# Global Settings
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "======================================")
message(STATUS "ABTRDA3 Configuration")
message(STATUS "======================================")
message(STATUS "Processor:     ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Compiler:  ${CMAKE_CXX_COMPILER}")
message(STATUS "======================================")

# =============================================================================
# Compiler Flags
# =============================================================================

add_library(abtrda3_flags INTERFACE)

target_compile_options(abtrda3_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -Wconversion
    -Wshadow
)

# Architecture-specific flags
if(CMAKE_CROSSCOMPILING)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        target_compile_options(abtrda3_flags INTERFACE -march=armv8-a)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(abtrda3_flags INTERFACE -march=x86-64 -mtune=generic)
    endif()
else()
    target_compile_options(abtrda3_flags INTERFACE -march=native -mtune=native)
endif()

# Release-specific flags
target_compile_options(abtrda3_flags INTERFACE
    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG
        -flto
    >
)

# Debug-specific flags
target_compile_options(abtrda3_flags INTERFACE
    $<$<CONFIG:Debug>:
        -O0
        -g3
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    >
)

target_link_options(abtrda3_flags INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Release>:-flto>
)

# =============================================================================
# Library
# =============================================================================

add_subdirectory(src)

# =============================================================================
# Tests
# =============================================================================

option(ABTRDA3_BUILD_TESTS "Build test executables" ON)

if(ABTRDA3_BUILD_TESTS)
    add_subdirectory(test)
endif()

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  - abtrda3      : Packet MMAP library (STATIC)")
if(ABTRDA3_BUILD_TESTS)
    message(STATUS "  - abtrda3_test : Ping-pong / burst test binary")
endif()
message(STATUS "")
