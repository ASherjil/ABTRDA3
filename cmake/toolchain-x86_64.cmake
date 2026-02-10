# =============================================================================
# x86_64 Cross-Compilation Toolchain (CERN CDK Debian 12)
# =============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# CERN CDK toolchain paths
set(TOOLCHAIN_ROOT "/acc/sys/cdk/debian/12/x86_64/sysroots/host/usr/bin")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/x86_64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/x86_64-linux-gnu-g++")

# Use toolchain's own linker (system /bin/ld is too old for Debian 12 sysroot)
set(CMAKE_LINKER        "${TOOLCHAIN_ROOT}/x86_64-linux-gnu-ld")

# LTO requires gcc-ar and gcc-ranlib (they have the LTO plugin)
set(CMAKE_AR            "${TOOLCHAIN_ROOT}/x86_64-linux-gnu-gcc-ar")
set(CMAKE_RANLIB        "${TOOLCHAIN_ROOT}/x86_64-linux-gnu-gcc-ranlib")

# Tell GCC to find its binutils (linker, assembler) from the toolchain
add_compile_options(-B${TOOLCHAIN_ROOT})
add_link_options(-B${TOOLCHAIN_ROOT})

# Search paths for cross-compilation
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
