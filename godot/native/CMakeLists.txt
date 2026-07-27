cmake_minimum_required(VERSION 3.22)
project(bk2_snapshot_fixture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BK2_ANDROID_CPP
    "${CMAKE_CURRENT_LIST_DIR}/../../android/app/src/main/cpp")

add_library(bk2_presentation_contract STATIC
    "${BK2_ANDROID_CPP}/bk2_presentation_api.cpp"
)
target_include_directories(bk2_presentation_contract PUBLIC "${BK2_ANDROID_CPP}")

add_executable(bk2_snapshot_fixture generate_fixture.cpp)
target_link_libraries(bk2_snapshot_fixture PRIVATE bk2_presentation_contract)

enable_testing()
add_executable(bk2_snapshot_api_test snapshot_api_test.cpp)
target_link_libraries(bk2_snapshot_api_test PRIVATE bk2_presentation_contract)
add_test(
    NAME bk2_snapshot_api_test
    COMMAND bk2_snapshot_api_test
        "${CMAKE_CURRENT_BINARY_DIR}/snapshot_api_test.json"
)
