cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE include)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(orderbook_tests test/OrderBookTest.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(orderbook_tests)
