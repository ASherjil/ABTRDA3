# =============================================================================
# ABTRDA3 — Ultra-Low Latency Network Library
# =============================================================================

cmake_minimum_required(VERSION 3.21)

project(ABTRDA3
        VERSION 0.1.0
        DESCRIPTION "Multi-transport low-latency networking for WREN timing sidecar"
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
if(CMAKE_CXX_COMPILER MATCHES "aarch64-linux-gnu")
    target_compile_options(abtrda3_flags INTERFACE -march=armv8-a+crc)
elseif(CMAKE_CXX_COMPILER MATCHES "x86_64-linux-gnu")
    target_compile_options(abtrda3_flags INTERFACE -march=x86-64-v3 -mtune=generic)
else()
    target_compile_options(abtrda3_flags INTERFACE -march=native -mtune=native)
endif()

# Release-specific flags
target_compile_options(abtrda3_flags INTERFACE
    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG
        -flto=auto
        -funroll-loops
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
# Dependencies
# =============================================================================

include(FetchContent)

# ABTEdge — kernel-bypass hardware access (Intel I210 DMA, hugepages)
FetchContent_Declare(abtedge
    GIT_REPOSITORY https://github.com/ASherjil/ABTEdge.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(abtedge)

# {fmt} — modern formatting library (fmt::print, fmt::format) until C++23 std::print lands
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        12.1.0
)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST    OFF CACHE BOOL "" FORCE)
set(FMT_DOC     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)

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
message(STATUS "  - abtrda3      : Multi-transport networking library (STATIC)")
if(ABTRDA3_BUILD_TESTS)
    message(STATUS "  - abtrda3_test : Ping-pong / burst test binary")
endif()
message(STATUS "")
