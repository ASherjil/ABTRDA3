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

    # AF_XDP transport is header-only (AFXDP.hpp); no .cpp here.
)

target_include_directories(abtrda3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/PACKET_MMAP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/AF_XDP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Intel_I210>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Cadence_GEM>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/TapBridge>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/DPDK>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Verbs>
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

# 3. Link libbpf (transitively pulls in -lelf -lz). PUBLIC: AFXDP.hpp is
#    header-only and emits libbpf calls (bpf_xdp_detach, bpf_map_update_elem,
#    bpf_object__*, bpf_map__fd) into every consumer's translation unit, so they
#    need libbpf's include path and symbols at link time too.
target_link_libraries(abtrda3 PUBLIC libbpf::libbpf)

# 4. libxdp — system-installed (libxdp-dev)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)
target_link_libraries(abtrda3 PUBLIC PkgConfig::LIBXDP)

# =============================================================================
# DPDK transport — header-only DPDK.hpp over libdpdk (PMDs i40e/igc/mlx5).
# Required dependency (like libxdp): install DPDK 25.11 LTS, plus rdma-core for
# the Mellanox/mlx5 (ConnectX-4) PMD. PUBLIC: DPDK.hpp emits rte_* calls into
# every consumer, so they need libdpdk's include path + symbols at link time
# (same reason libbpf/libxdp are PUBLIC).
# =============================================================================
pkg_check_modules(LIBDPDK REQUIRED IMPORTED_TARGET libdpdk)
target_link_libraries(abtrda3 PUBLIC PkgConfig::LIBDPDK)
message(STATUS "DPDK transport: libdpdk ${LIBDPDK_VERSION}")

# =============================================================================
# Verbs transport — header-only Verbs.hpp over libibverbs (RAW_PACKET QP,
# mlx5/RDMA-capable NICs only; rdma-core/libibverbs-dev). PUBLIC for the same
# reason as libdpdk: the header emits ibv_* calls into its consumers.
# =============================================================================
pkg_check_modules(LIBIBVERBS REQUIRED IMPORTED_TARGET libibverbs)
target_link_libraries(abtrda3 PUBLIC PkgConfig::LIBIBVERBS)
message(STATUS "Verbs transport: libibverbs ${LIBIBVERBS_VERSION}")
pkg_check_modules(LIBMLX5 REQUIRED IMPORTED_TARGET libmlx5)
target_link_libraries(abtrda3 PUBLIC PkgConfig::LIBMLX5)
message(STATUS "Verbs transport: libmlx5 ${LIBMLX5_VERSION} (mlx5dv direct CQE poll)")

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
