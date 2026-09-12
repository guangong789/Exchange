cmake_minimum_required(VERSION 3.20)

project(crypto_exchange_simulator VERSION 1.0.0 LANGUAGES CXX)

option(EXCHANGE_BUILD_TESTS "Build unit tests" ON)
option(EXCHANGE_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(EXCHANGE_BUILD_DEEPSEEK_PROVIDER "Build the DeepSeek agent provider" ON)
option(EXCHANGE_BUILD_BINANCE_ALPHA_FEED "Build the Binance Alpha market feed" ON)
find_package(Threads REQUIRED)

add_library(exchange_core
    src/accounting/account_store.cpp
    src/accounting/execution_coordinator.cpp
    src/accounting/financial_conversion.cpp
    src/accounting/funding_coordinator.cpp
    src/accounting/ledger.cpp
    src/execution/execution_command_applier.cpp
    src/execution/execution_sequencer.cpp
    src/execution/trading_request_admission.cpp
    src/execution/trading_request_executor.cpp
    src/execution/trading_runtime.cpp
    src/execution/trading_bootstrap.cpp
    src/matching/matching_engine.cpp
    src/matching/order_book.cpp
    src/accounting/order_reservation_store.cpp
    src/durability/execution_wal.cpp
    src/durability/execution_recovery.cpp
    src/durability/execution_wal_writer.cpp
    src/replay/replay_engine.cpp
    src/replay/workload_generator.cpp
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
    src/protocol/line_protocol.cpp
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
        src/gateway/epoll_server.cpp
        src/gateway/tcp_gateway.cpp
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
        apps/exchange_server/main.cpp
    )
    target_link_libraries(exchange_server PRIVATE exchange::gateway)
    target_compile_features(exchange_server PRIVATE cxx_std_20)
    target_compile_options(exchange_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Provider-independent agent components use the exchange through explicit adapters.
add_library(exchange_agent
    src/agent/strategy/rule_based_policy.cpp
    src/agent/domain/action_validation.cpp
    src/agent/domain/external_market.cpp
    src/agent/domain/agent_registry.cpp
    src/agent/exchange/trading_request_agent_execution_adapter.cpp
    src/agent/exchange/agent_observation_service.cpp
    src/agent/exchange/agent_objective.cpp
    src/agent/runtime/agent_runtime.cpp
)
add_library(exchange::agent ALIAS exchange_agent)
target_link_libraries(exchange_agent PUBLIC exchange::core)
target_compile_features(exchange_agent PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(exchange_agent PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_agent PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EXCHANGE_BUILD_BINANCE_ALPHA_FEED)
    find_package(CURL REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(Boost 1.74 CONFIG REQUIRED)

    add_library(exchange_agent_binance_alpha
        src/agent/provider/binance_alpha/binance_alpha_market_feed.cpp
    )
    add_library(exchange::agent_binance_alpha
        ALIAS exchange_agent_binance_alpha)
    target_link_libraries(exchange_agent_binance_alpha
        PUBLIC exchange::agent
        PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
    )
    target_compile_definitions(exchange_agent_binance_alpha
        PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
    target_compile_features(exchange_agent_binance_alpha PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(
            exchange_agent_binance_alpha PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            exchange_agent_binance_alpha PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(EXCHANGE_BUILD_DEEPSEEK_PROVIDER)
    find_package(CURL REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)

    add_library(exchange_agent_deepseek
        src/agent/provider/deepseek/deepseek_client.cpp
        src/agent/provider/deepseek/deepseek_decision_provider.cpp
    )
    add_library(exchange::agent_deepseek ALIAS exchange_agent_deepseek)
    target_link_libraries(exchange_agent_deepseek
        PUBLIC exchange::agent
        PRIVATE CURL::libcurl nlohmann_json::nlohmann_json
    )
    target_compile_features(exchange_agent_deepseek PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(
            exchange_agent_deepseek PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            exchange_agent_deepseek PRIVATE -Wall -Wextra -Wpedantic)
    endif()
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

        add_executable(exchange_recovery_benchmark
            benchmarks/recovery_benchmark.cpp
        )
        target_link_libraries(exchange_recovery_benchmark
            PRIVATE exchange::core
        )
        target_compile_features(exchange_recovery_benchmark PRIVATE cxx_std_20)
        target_compile_options(exchange_recovery_benchmark
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
        tests/accounting/account_reservation_integration_test.cpp
        tests/accounting/account_store_test.cpp
        tests/core/bounded_queue_test.cpp
        tests/durability/execution_wal_test.cpp
        tests/durability/execution_wal_writer_test.cpp
        tests/durability/durable_execution_test.cpp
        tests/durability/execution_recovery_test.cpp
        tests/execution/execution_command_test.cpp
        tests/execution/trading_bootstrap_test.cpp
        tests/execution/trading_request_executor_test.cpp
        tests/execution/trading_runtime_test.cpp
        tests/accounting/execution_coordinator_test.cpp
        tests/matching/event_system_test.cpp
        tests/accounting/financial_conversion_test.cpp
        tests/accounting/funding_coordinator_test.cpp
        tests/protocol/line_protocol_test.cpp
        tests/accounting/ledger_test.cpp
        tests/matching/order_book_test.cpp
        tests/accounting/order_reservation_store_test.cpp
        tests/replay/replay_engine_test.cpp
        tests/replay/workload_generator_test.cpp
    )
    list(APPEND EXCHANGE_TEST_SOURCES
        tests/agent/exchange/agent_foundation_test.cpp
        tests/agent/exchange/durable_agent_execution_test.cpp
        tests/agent/exchange/agent_objective_test.cpp
        tests/agent/domain/action_validation_test.cpp
        tests/agent/runtime/agent_runtime_test.cpp)
    if(TARGET exchange_agent_binance_alpha)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/agent/provider/binance_alpha_market_feed_test.cpp)
    endif()
    if(TARGET exchange_agent_deepseek)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/agent/provider/deepseek_decision_provider_test.cpp)
    endif()
    if(TARGET exchange_gateway)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/gateway/epoll_server_test.cpp
            tests/gateway/durable_server_process_test.cpp
            tests/gateway/tcp_gateway_test.cpp
        )
    endif()
    add_executable(exchange_test ${EXCHANGE_TEST_SOURCES})
    target_link_libraries(exchange_test
        PRIVATE
            exchange::core
            exchange::agent
            exchange::protocol
            GTest::gtest_main
            Threads::Threads
    )
    if(TARGET exchange_agent_deepseek)
        target_link_libraries(exchange_test PRIVATE exchange::agent_deepseek)
    endif()
    if(TARGET exchange_agent_binance_alpha)
        target_link_libraries(exchange_test
            PRIVATE exchange::agent_binance_alpha)
    endif()
    if(TARGET exchange_gateway)
        target_link_libraries(exchange_test PRIVATE exchange::gateway)
        add_dependencies(exchange_test exchange_server)
        target_compile_definitions(exchange_test PRIVATE
            EXCHANGE_SERVER_PATH="$<TARGET_FILE:exchange_server>"
        )
    endif()
    target_compile_features(exchange_test PRIVATE cxx_std_20)

    include(GoogleTest)
    gtest_discover_tests(exchange_test)
endif()
