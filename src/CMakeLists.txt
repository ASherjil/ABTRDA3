add_library(abtrda3 STATIC
    SocketOps.cpp
    PacketMmapRx.cpp
    PacketMmapTx.cpp
    NicTuner.cpp
)

target_include_directories(abtrda3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(abtrda3 PUBLIC cxx_std_20)

target_link_libraries(abtrda3 PRIVATE abtrda3_flags)

# =============================================================================
# AF_XDP Transport (conditional on ABTRDA3_ENABLE_AF_XDP)
# =============================================================================

if(ABTRDA3_ENABLE_AF_XDP)
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

    # 3. AF_XDP source files
    target_sources(abtrda3 PRIVATE
            AFXDPSocket.cpp
            AFXDPTx.cpp
            AFXDPRx.cpp
    )

    # 4. Link libbpf (transitively pulls in -lelf -lz)
    target_link_libraries(abtrda3 PRIVATE libbpf::libbpf)

    # 5. Public define so downstream code can #ifdef on transport availability
    target_compile_definitions(abtrda3 PUBLIC ABTRDA3_HAS_AF_XDP=1)
endif()
