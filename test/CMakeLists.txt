include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# rigtorp SPSCQueue — header-only lock-free single-producer/single-consumer queue
# (carries raw TSC cycle-deltas from the hot path to the recorder thread).
FetchContent_Declare(
    SPSCQueue
    GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(SPSCQueue)

# HdrHistogram_c — fixed-memory high-dynamic-range latency histogram. Records
# every sample (billions) into ~1.5 MB so a 24 h soak needs no GB-scale storage.
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(HDR_LOG_REQUIRED             OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.11.8
)
FetchContent_MakeAvailable(hdr_histogram)

add_executable(abtrda3_test main.cpp)

target_link_libraries(abtrda3_test PRIVATE
    abtrda3 abtrda3_flags tomlplusplus::tomlplusplus
    SPSCQueue::SPSCQueue hdr_histogram_static)

# TapBridge standalone test — validates TAP fd plumbing without NIC hardware
add_executable(tap_bridge_test tap_bridge_test.cpp)

target_link_libraries(tap_bridge_test PRIVATE abtrda3 abtrda3_flags)
