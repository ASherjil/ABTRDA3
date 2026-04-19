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
    AF_XDP/AFXDPTx.cpp
    AF_XDP/AFXDPRx.cpp
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
)

# =============================================================================
# AF_XDP: libbpf + BPF program compilation
# =============================================================================

# 1. Fetch + build libbpf (provides libbpf::libbpf IMPORTED target)
include(${CMAKE_SOURCE_DIR}/cmake/Libbpf.cmake)

# 2. Compile XDP filter program and embed as C header
include(${CMAKE_SOURCE_DIR}/cmake/BpfCompile.cmake)
bpf_compile(
    TARGET     abtrda3
    SOURCE     "${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_filter.bpf.c"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bpf"
    INCLUDES   "${libbpf_SOURCE_DIR}/src"
)

# 3. Link libbpf (transitively pulls in -lelf -lz)
target_link_libraries(abtrda3 PRIVATE libbpf::libbpf)
