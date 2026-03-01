include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

add_executable(abtrda3_test main.cpp)

target_link_libraries(abtrda3_test PRIVATE abtrda3 abtrda3_flags tomlplusplus::tomlplusplus)
