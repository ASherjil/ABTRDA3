# =============================================================================
# ARM64 Cross-Compilation Toolchain (CERN CDK Debian 12)
# =============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# CERN CDK toolchain paths
set(TOOLCHAIN_ROOT "/acc/sys/cdk/debian/12/aarch64/sysroots/host/usr/bin")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/aarch64-linux-gnu-g++")

# LTO requires gcc-ar and gcc-ranlib (they have the LTO plugin)
set(CMAKE_AR            "${TOOLCHAIN_ROOT}/aarch64-linux-gnu-gcc-ar")
set(CMAKE_RANLIB        "${TOOLCHAIN_ROOT}/aarch64-linux-gnu-gcc-ranlib")

# Target sysroot (contains ARM64 libraries and headers)
set(CMAKE_SYSROOT "/acc/sys/cdk/debian/12/aarch64/sysroots/target")

# Search paths for cross-compilation
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
