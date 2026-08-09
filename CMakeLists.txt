cmake_minimum_required(VERSION 3.20)

project(crypto_exchange_simulator VERSION 0.1.0 LANGUAGES CXX)

option(EXCHANGE_BUILD_TESTS "Build unit tests" ON)

add_library(exchange_core
    src/order_book.cpp
)
add_library(exchange::core ALIAS exchange_core)

target_compile_features(exchange_core PUBLIC cxx_std_20)
target_include_directories(exchange_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(exchange_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EXCHANGE_BUILD_TESTS)
    include(CTest)
    enable_testing()

    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(exchange_test
        tests/order_book_test.cpp
    )
    target_link_libraries(exchange_test PRIVATE exchange::core GTest::gtest_main)
    target_compile_features(exchange_test PRIVATE cxx_std_20)

    include(GoogleTest)
    gtest_discover_tests(exchange_test)
endif()
