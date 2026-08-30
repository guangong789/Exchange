cmake_minimum_required(VERSION 3.20)

project(crypto_exchange_simulator VERSION 1.0.0 LANGUAGES CXX)

option(EXCHANGE_BUILD_TESTS "Build unit tests" ON)
option(EXCHANGE_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

find_package(Threads REQUIRED)

add_library(exchange_core
    src/account_store.cpp
    src/execution_coordinator.cpp
    src/financial_conversion.cpp
    src/funding_coordinator.cpp
    src/ledger.cpp
    src/matching_engine.cpp
    src/order_book.cpp
    src/order_reservation_store.cpp
    src/replay_engine.cpp
    src/workload_generator.cpp
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

add_library(exchange_protocol
    src/line_protocol.cpp
)
add_library(exchange::protocol ALIAS exchange_protocol)

target_link_libraries(exchange_protocol PUBLIC exchange::core)
target_compile_features(exchange_protocol PUBLIC cxx_std_20)
target_include_directories(exchange_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(exchange_protocol PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(exchange_gateway
        src/epoll_server.cpp
        src/tcp_gateway.cpp
    )
    add_library(exchange::gateway ALIAS exchange_gateway)

    target_link_libraries(exchange_gateway
        PUBLIC exchange::protocol
        PRIVATE Threads::Threads
    )
    target_compile_features(exchange_gateway PUBLIC cxx_std_20)
    target_include_directories(exchange_gateway
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_options(exchange_gateway PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(exchange_server
        apps/gateway_main.cpp
    )
    target_link_libraries(exchange_server PRIVATE exchange::gateway)
    target_compile_features(exchange_server PRIVATE cxx_std_20)
    target_compile_options(exchange_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EXCHANGE_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)

    add_executable(exchange_benchmark
        benchmarks/exchange_benchmark.cpp
    )
    target_link_libraries(exchange_benchmark
        PRIVATE
            exchange::core
            benchmark::benchmark
    )
    target_compile_features(exchange_benchmark PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(exchange_benchmark PRIVATE /W4 /permissive-)
    else()
        target_compile_options(exchange_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(TARGET exchange_gateway)
        add_executable(exchange_gateway_benchmark
            benchmarks/gateway_benchmark.cpp
        )
        target_link_libraries(exchange_gateway_benchmark
            PRIVATE
                exchange::gateway
                Threads::Threads
        )
        target_compile_features(exchange_gateway_benchmark PRIVATE cxx_std_20)
        target_compile_options(exchange_gateway_benchmark
            PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()
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

    set(EXCHANGE_TEST_SOURCES
        tests/account_reservation_integration_test.cpp
        tests/account_store_test.cpp
        tests/bounded_queue_test.cpp
        tests/execution_coordinator_test.cpp
        tests/event_system_test.cpp
        tests/financial_conversion_test.cpp
        tests/funding_coordinator_test.cpp
        tests/line_protocol_test.cpp
        tests/ledger_test.cpp
        tests/order_book_test.cpp
        tests/order_reservation_store_test.cpp
        tests/replay_engine_test.cpp
        tests/workload_generator_test.cpp
    )
    if(TARGET exchange_gateway)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/epoll_server_test.cpp
            tests/tcp_gateway_test.cpp
        )
    endif()

    add_executable(exchange_test ${EXCHANGE_TEST_SOURCES})
    target_link_libraries(exchange_test
        PRIVATE
            exchange::core
            exchange::protocol
            GTest::gtest_main
            Threads::Threads
    )
    if(TARGET exchange_gateway)
        target_link_libraries(exchange_test PRIVATE exchange::gateway)
    endif()
    target_compile_features(exchange_test PRIVATE cxx_std_20)

    include(GoogleTest)
    gtest_discover_tests(exchange_test)
endif()
