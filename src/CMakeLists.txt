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
