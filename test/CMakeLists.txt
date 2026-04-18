include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

add_executable(abtrda3_test main.cpp)

target_link_libraries(abtrda3_test PRIVATE abtrda3 abtrda3_flags tomlplusplus::tomlplusplus)

# TapBridge standalone test — validates TAP fd plumbing without NIC hardware
add_executable(tap_bridge_test tap_bridge_test.cpp)

target_link_libraries(tap_bridge_test PRIVATE abtrda3 abtrda3_flags)
