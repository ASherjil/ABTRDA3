# =============================================================================
# abtrda3 — static library with all transports
# =============================================================================

add_library(abtrda3 STATIC
    # Common
    common/NicTuner.cpp

    # PACKET_MMAP transport
    PACKET_MMAP/SocketOps.cpp
    PACKET_MMAP/PacketMmapRx.cpp
    PACKET_MMAP/PacketMmapTx.cpp

    # AF_XDP transport
    AF_XDP/AFXDPSocket.cpp
)

target_include_directories(abtrda3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/PACKET_MMAP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/AF_XDP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Intel_I210>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Cadence_GEM>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/TapBridge>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(abtrda3 PUBLIC cxx_std_20)

target_link_libraries(abtrda3
    PRIVATE abtrda3_flags
    PUBLIC  abtedge_backend
    PUBLIC  fmt::fmt
)

# =============================================================================
# AF_XDP: libbpf + BPF program compilation
# =============================================================================

# 1. Fetch + build libbpf (provides libbpf::libbpf IMPORTED target)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Libbpf.cmake)

# 2. Compile XDP filter program and embed as C header
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BpfCompile.cmake)
bpf_compile(
    TARGET     abtrda3
    SOURCE     "${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_filter.bpf.c"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bpf"
    INCLUDES   "${libbpf_SOURCE_DIR}/src"
)

# 3. Link libbpf (transitively pulls in -lelf -lz)
target_link_libraries(abtrda3 PRIVATE libbpf::libbpf)

# 4. libxdp — system-installed (libxdp-dev)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)
target_link_libraries(abtrda3 PUBLIC PkgConfig::LIBXDP)

# 5. Compile AF_XDP BPF filter (.o file loaded at runtime xdpsock-style)
#    Output to both src build dir and test build dir.
set(BPF_KERN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/AF_XDP/af_xdp_kern.c")
set(BPF_KERN_DST "${CMAKE_BINARY_DIR}/test/af_xdp_kern.o")
add_custom_command(
    OUTPUT "${BPF_KERN_DST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test"
    COMMAND clang -O2 -target bpf -g -c "${BPF_KERN_SRC}" -o "${BPF_KERN_DST}"
            -I${libbpf_SOURCE_DIR}/src
    DEPENDS "${BPF_KERN_SRC}"
    COMMENT "[BPF] af_xdp_kern.c -> af_xdp_kern.o"
)
add_custom_target(af_xdp_bpf DEPENDS "${BPF_KERN_DST}")
add_dependencies(abtrda3 af_xdp_bpf)
