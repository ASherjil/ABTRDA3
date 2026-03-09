# =============================================================================
# ABTRDA3 — Ultra-Low Latency Packet MMAP Library
# =============================================================================

cmake_minimum_required(VERSION 3.21)

project(ABTRDA3
        VERSION 0.1.0
        DESCRIPTION "packet_mmap TX/RX for WREN timing sidecar"
        LANGUAGES CXX C
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
# Detect from compiler path so this works even when CLion sets the compiler
# directly without a toolchain file (CMAKE_CROSSCOMPILING not set).
if(CMAKE_CXX_COMPILER MATCHES "aarch64-linux-gnu")
    target_compile_options(abtrda3_flags INTERFACE -march=armv8-a)
elseif(CMAKE_CXX_COMPILER MATCHES "x86_64-linux-gnu")
    target_compile_options(abtrda3_flags INTERFACE -march=x86-64 -mtune=generic)
else()
    # Native compiler — optimise for the host CPU
    target_compile_options(abtrda3_flags INTERFACE -march=native -mtune=native)
endif()

# Release-specific flags
target_compile_options(abtrda3_flags INTERFACE
    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG
        -flto=auto
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
    $<$<CONFIG:Release>:-flto=auto>
)

# =============================================================================
# AF_XDP Transport (optional)
# =============================================================================

option(ABTRDA3_ENABLE_AF_XDP "Enable AF_XDP transport (requires clang, libelf, zlib)" OFF)

if(ABTRDA3_ENABLE_AF_XDP)
    message(STATUS "AF_XDP transport: ENABLED")
else()
    message(STATUS "AF_XDP transport: disabled")
endif()

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
if(ABTRDA3_ENABLE_AF_XDP)
    message(STATUS "  - abtrda3      : Packet MMAP + AF_XDP library (STATIC)")
else()
    message(STATUS "  - abtrda3      : Packet MMAP library (STATIC)")
endif()
if(ABTRDA3_BUILD_TESTS)
    message(STATUS "  - abtrda3_test : Ping-pong / burst test binary")
endif()
message(STATUS "")
