# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Areeb Sherjil

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

# Opt-in hot-path profiling (rdtsc CPU-cost + mlx5 RX HW-timestamp). OFF by
# default so PRODUCTION builds carry ZERO instrumentation; the x86_64-relprof
# preset turns it ON (Release optimisation + ABT_DEBUG_PROFILING) in its OWN
# build dir, leaving the release binary untouched. See backends/common/Profiling.hpp.
option(ABTRDA3_DEBUG_PROFILING "Enable hot-path rdtsc/HW-ts profiling instrumentation" OFF)
target_compile_definitions(abtrda3_flags INTERFACE
    $<$<BOOL:${ABTRDA3_DEBUG_PROFILING}>:ABT_DEBUG_PROFILING>
)
if(ABTRDA3_DEBUG_PROFILING)
    message(STATUS "Profiling:     ON (ABT_DEBUG_PROFILING) — debug measurement build")
endif()

# =============================================================================
# Optional transports
# =============================================================================
# packet_mmap + af_xdp ALWAYS build — they only need what every mainstream
# distro ships (AF_PACKET is in the kernel; AF_XDP needs libxdp + clang).
# The rest sit on heavyweight vendor stacks, so each is opt-in; ABTRDA3_WITH_ALL
# turns on all four at once (the rtserver / full-benchmark build).
option(ABTRDA3_WITH_ALL   "Build every optional transport (dpdk, verbs, ef_vi, intel_i210); overrides the individual options" OFF)
option(ABTRDA3_WITH_DPDK  "Build the DPDK transport (needs libdpdk)" OFF)
option(ABTRDA3_WITH_VERBS "Build the Verbs transport (needs rdma-core: libibverbs + libmlx5)" OFF)
option(ABTRDA3_WITH_EFVI  "Build the ef_vi transport (needs Onload: libciul + etherfabric headers)" OFF)
option(ABTRDA3_WITH_I210  "Build the Intel I210 register PMD" OFF)

# Plain set(), NOT "CACHE ... FORCE": WITH_ALL is a per-configure override.
# Flipping it back OFF reverts to the individually cached options instead of
# leaving all four permanently forced ON in the cache.
if(ABTRDA3_WITH_ALL)
    set(ABTRDA3_WITH_DPDK  ON)
    set(ABTRDA3_WITH_VERBS ON)
    set(ABTRDA3_WITH_EFVI  ON)
    set(ABTRDA3_WITH_I210  ON)
endif()

message(STATUS "Transports:    packet_mmap af_xdp (always) | dpdk=${ABTRDA3_WITH_DPDK} verbs=${ABTRDA3_WITH_VERBS} ef_vi=${ABTRDA3_WITH_EFVI} intel_i210=${ABTRDA3_WITH_I210}")

# =============================================================================
# Dependencies
# =============================================================================

include(FetchContent)

# ABTEdge — kernel-bypass hardware access. Needed by EVERY build (not just the
# I210 PMD): common/PciHelpers.hpp uses its BackendBase sysfs-resource0 MMIO for
# the i40e ITR/NoITR pokes and the igc EEE clear.
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

add_subdirectory(src/backends)

# =============================================================================
# Application layer + test executables
# =============================================================================

option(ABTRDA3_BUILD_TESTS "Build test executables" ON)

if(ABTRDA3_BUILD_TESTS)
    add_subdirectory(src/app)
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
