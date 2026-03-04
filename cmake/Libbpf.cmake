# =============================================================================
# Libbpf.cmake — Fetch and build libbpf v1.5.0 as a static library
# =============================================================================
#
# Provides:  libbpf::libbpf  IMPORTED STATIC target
# Requires:  make, libelf-dev, zlib on the system
# Also sets: libbpf_SOURCE_DIR (for BPF program compilation includes)

include(FetchContent)

FetchContent_Declare(libbpf
    GIT_REPOSITORY https://github.com/libbpf/libbpf.git
    GIT_TAG        v1.5.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libbpf)

set(LIBBPF_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/libbpf_install")
set(LIBBPF_LIB     "${LIBBPF_INSTALL}/lib/libbpf.a")
set(LIBBPF_INCLUDE "${LIBBPF_INSTALL}/include")

# Create include dir at configure time -- CMake validates that
# INTERFACE_INCLUDE_DIRECTORIES paths exist on IMPORTED targets
file(MAKE_DIRECTORY "${LIBBPF_INCLUDE}")

# Build libbpf using its own Makefile.
# This handles libbpf_version.h generation and all internal edge cases.
add_custom_command(
    OUTPUT  "${LIBBPF_LIB}"
    COMMAND make
            -C "${libbpf_SOURCE_DIR}/src"
            BUILD_STATIC_ONLY=1
            "OBJDIR=${CMAKE_CURRENT_BINARY_DIR}/libbpf_obj"
            "DESTDIR=${LIBBPF_INSTALL}"
            INCLUDEDIR=/include
            LIBDIR=/lib
            UAPIDIR=/include/uapi
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "CFLAGS=-O3 -fPIC"
            install
    COMMENT "Building libbpf v1.5.0 (static)"
    VERBATIM
)

add_custom_target(libbpf_build DEPENDS "${LIBBPF_LIB}")

# IMPORTED target for clean dependency tracking.
# INTERFACE_LINK_LIBRARIES propagates -lelf -lz to anything linking libbpf.
add_library(libbpf::libbpf STATIC IMPORTED GLOBAL)
set_target_properties(libbpf::libbpf PROPERTIES
    IMPORTED_LOCATION             "${LIBBPF_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBBPF_INCLUDE}"
    INTERFACE_LINK_LIBRARIES      "elf;z"
)
add_dependencies(libbpf::libbpf libbpf_build)
