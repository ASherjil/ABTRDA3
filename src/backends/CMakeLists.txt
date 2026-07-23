# =============================================================================
# abtrda3 — static library with all transports
# =============================================================================

add_library(abtrda3 STATIC
    # Common
    common/NicTuner.cpp

    # PACKET_MMAP transport
        PacketMmap/SocketOps.cpp
        PacketMmap/PacketMmapRx.cpp
        PacketMmap/PacketMmapTx.cpp

    # DPDK transport: the hot path is header-only (DPDK.hpp, a template), but the
    # process-global EAL base is a plain class -> compiled ONCE here.
        DPDK/DPDKEal.cpp

    # AF_XDP transport is header-only (AFXDP.hpp); no .cpp here.
)

target_include_directories(abtrda3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/PacketMmap>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/AF_XDP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/IGBCustomPMD>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/DPDK>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Verbs>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/EtherFabricVirtualInterface>
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
include(${CMAKE_SOURCE_DIR}/cmake/Libbpf.cmake)

# 2. Compile XDP filter program and embed as C header
include(${CMAKE_SOURCE_DIR}/cmake/BpfCompile.cmake)
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

# =============================================================================
# ef_vi transport — header-only EtherFabricVirtualInterface.hpp over libciul
# (Solarflare/AMD kernel bypass, Onload 9.0.2, X2522). Onload installs
# libciul1.a + /usr/include/etherfabric WITHOUT a pkg-config file, so this is
# find_library/find_path rather than the pkg_check_modules used above. PUBLIC
# for the same reason as libdpdk/libibverbs: the header emits ef_* calls into
# every consumer. SYSTEM keeps the C headers clear of -Wconversion/-Wpedantic.
# =============================================================================
# HINTS: a sibling onload checkout serves machines where Onload is built but
# not installed (workstation); on installed systems (rtserver) the hints miss
# and the system paths (/usr/include, /usr/lib) resolve as before.
find_path(ETHERFABRIC_INCLUDE_DIR etherfabric/ef_vi.h
          HINTS ${CMAKE_SOURCE_DIR}/../onload/src/include REQUIRED)
find_library(LIBCIUL_LIBRARY NAMES ciul1 ciul
             HINTS ${CMAKE_SOURCE_DIR}/../onload/build/gnu_x86_64/lib/ciul REQUIRED)
target_include_directories(abtrda3 SYSTEM PUBLIC ${ETHERFABRIC_INCLUDE_DIR})
target_link_libraries(abtrda3 PUBLIC ${LIBCIUL_LIBRARY})
message(STATUS "ef_vi transport: ${LIBCIUL_LIBRARY}")

# 5. Compile AF_XDP BPF filter (.o file loaded at runtime xdpsock-style).
#    Output lands NEXT TO the abtrda3_test executable (build/src/app) — the
#    transport loads "af_xdp_kern.o" relative to the run directory.
set(BPF_KERN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/AF_XDP/af_xdp_kern.c")
set(BPF_KERN_DST "${CMAKE_BINARY_DIR}/src/app/af_xdp_kern.o")
# Same multiarch include as BpfCompile.cmake: clang -target bpf drops the host
# default paths where <asm/types.h> lives on Debian-family systems.
set(BPF_KERN_INCLUDES "-I${libbpf_SOURCE_DIR}/src")
if(EXISTS "/usr/include/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu")
    list(APPEND BPF_KERN_INCLUDES "-I/usr/include/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu")
endif()
add_custom_command(
    OUTPUT "${BPF_KERN_DST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/src/app"
    COMMAND clang -O2 -target bpf -g -c "${BPF_KERN_SRC}" -o "${BPF_KERN_DST}"
            ${BPF_KERN_INCLUDES}
    DEPENDS "${BPF_KERN_SRC}"
    COMMENT "[BPF] af_xdp_kern.c -> af_xdp_kern.o"
)
add_custom_target(af_xdp_bpf DEPENDS "${BPF_KERN_DST}")
add_dependencies(abtrda3 af_xdp_bpf)
