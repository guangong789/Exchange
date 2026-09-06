cmake_minimum_required(VERSION 3.20)

project(crypto_exchange_simulator VERSION 1.0.0 LANGUAGES CXX)

option(EXCHANGE_BUILD_TESTS "Build unit tests" ON)
option(EXCHANGE_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(EXCHANGE_ENABLE_LIVE_MODEL_TESTS "Enable live provider smoke tests" OFF)
option(EXCHANGE_ENABLE_BINANCE_AGENT_OS_TESTS
    "Enable live Binance Agent OS smoke test" OFF)
option(EXCHANGE_ENABLE_BINANCE_WALLET_TESTS
    "Enable live Binance Agentic Wallet preview smoke test" OFF)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(exchange_core
    src/accounting/account_store.cpp
    src/agent/agent.cpp
    src/agent/agent_action_gateway.cpp
    src/agent/agent_observation_service.cpp
    src/agent/agent_objective.cpp
    src/agent/agent_registry.cpp
    src/arena/arena.cpp
    src/accounting/execution_coordinator.cpp
    src/accounting/financial_conversion.cpp
    src/accounting/funding_coordinator.cpp
    src/accounting/ledger.cpp
    src/matching/matching_engine.cpp
    src/matching/order_book.cpp
    src/accounting/order_reservation_store.cpp
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

add_library(exchange_binance_agent_os
    src/binance/binance_agent_os_client.cpp
)
add_library(exchange::binance_agent_os ALIAS exchange_binance_agent_os)

target_link_libraries(exchange_binance_agent_os
    PUBLIC exchange::core
    PRIVATE CURL::libcurl nlohmann_json::nlohmann_json
)
target_compile_features(exchange_binance_agent_os PUBLIC cxx_std_20)
target_include_directories(exchange_binance_agent_os
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(exchange_binance_agent_os PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_binance_agent_os
        PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(exchange_model
    src/model/deepseek_adapter.cpp
    src/model/model_agent_policy.cpp
    src/model/model_decision.cpp
)
add_library(exchange::model ALIAS exchange_model)

target_link_libraries(exchange_model
    PUBLIC exchange::core
    PRIVATE CURL::libcurl nlohmann_json::nlohmann_json
)
target_compile_features(exchange_model PUBLIC cxx_std_20)
target_include_directories(exchange_model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(exchange_model PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_model PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(exchange_hackathon_demo_lib
    src/hackathon/hackathon_demo.cpp
    src/hackathon/live_x402_evidence.cpp
)
add_library(exchange::hackathon_demo ALIAS exchange_hackathon_demo_lib)

target_link_libraries(exchange_hackathon_demo_lib
    PUBLIC exchange::core exchange::model)
target_compile_features(exchange_hackathon_demo_lib PUBLIC cxx_std_20)
target_include_directories(exchange_hackathon_demo_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(exchange_hackathon_demo_lib PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_hackathon_demo_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(exchange_hackathon_demo
    tools/hackathon_demo_main.cpp
)
target_link_libraries(exchange_hackathon_demo
    PRIVATE exchange::hackathon_demo nlohmann_json::nlohmann_json)
target_compile_features(exchange_hackathon_demo PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(exchange_hackathon_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_hackathon_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(exchange_hackathon_ui_bridge
    tools/hackathon_ui_bridge_main.cpp
)
target_link_libraries(exchange_hackathon_ui_bridge
    PRIVATE exchange::hackathon_demo nlohmann_json::nlohmann_json)
target_compile_features(exchange_hackathon_ui_bridge PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(exchange_hackathon_ui_bridge PRIVATE /W4 /permissive-)
else()
    target_compile_options(exchange_hackathon_ui_bridge PRIVATE -Wall -Wextra -Wpedantic)
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
    add_library(exchange_x402
        src/x402/x402.cpp
    )
    add_library(exchange::x402 ALIAS exchange_x402)

    target_link_libraries(exchange_x402
        PRIVATE CURL::libcurl nlohmann_json::nlohmann_json
    )
    target_compile_features(exchange_x402 PUBLIC cxx_std_20)
    target_include_directories(exchange_x402
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_options(exchange_x402 PRIVATE -Wall -Wextra -Wpedantic)

    add_library(exchange_binance_agentic_wallet
        src/binance/binance_agentic_wallet.cpp
    )
    add_library(exchange::binance_agentic_wallet
        ALIAS exchange_binance_agentic_wallet)

    target_link_libraries(exchange_binance_agentic_wallet
        PUBLIC exchange::x402
        PRIVATE nlohmann_json::nlohmann_json
    )
    target_compile_features(exchange_binance_agentic_wallet PUBLIC cxx_std_20)
    target_include_directories(exchange_binance_agentic_wallet
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_options(exchange_binance_agentic_wallet
        PRIVATE -Wall -Wextra -Wpedantic)

    target_link_libraries(exchange_hackathon_demo_lib
        PUBLIC exchange::binance_agentic_wallet)

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
        tests/accounting/account_reservation_integration_test.cpp
        tests/accounting/account_store_test.cpp
        tests/agent/agent_foundation_test.cpp
        tests/agent/agent_objective_test.cpp
        tests/arena/arena_test.cpp
        tests/core/bounded_queue_test.cpp
        tests/binance/binance_agent_os_test.cpp
        tests/accounting/execution_coordinator_test.cpp
        tests/matching/event_system_test.cpp
        tests/accounting/financial_conversion_test.cpp
        tests/accounting/funding_coordinator_test.cpp
        tests/hackathon/hackathon_demo_test.cpp
        tests/hackathon/live_x402_evidence_test.cpp
        tests/protocol/line_protocol_test.cpp
        tests/accounting/ledger_test.cpp
        tests/model/model_agent_policy_test.cpp
        tests/model/model_decision_test.cpp
        tests/matching/order_book_test.cpp
        tests/accounting/order_reservation_store_test.cpp
        tests/replay/replay_engine_test.cpp
        tests/replay/workload_generator_test.cpp
    )
    if(TARGET exchange_gateway)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/gateway/epoll_server_test.cpp
            tests/gateway/tcp_gateway_test.cpp
        )
    endif()
    if(TARGET exchange_x402)
        list(APPEND EXCHANGE_TEST_SOURCES tests/x402/x402_test.cpp)
    endif()
    if(TARGET exchange_binance_agentic_wallet)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/binance/binance_agentic_wallet_test.cpp)
    endif()
    if(EXCHANGE_ENABLE_LIVE_MODEL_TESTS)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/model/deepseek_live_test.cpp
            tests/hackathon/hackathon_demo_live_test.cpp)
    endif()
    if(EXCHANGE_ENABLE_BINANCE_AGENT_OS_TESTS)
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/binance/binance_agent_os_live_test.cpp)
    endif()
    if(EXCHANGE_ENABLE_BINANCE_WALLET_TESTS)
        if(NOT TARGET exchange_binance_agentic_wallet)
            message(FATAL_ERROR
                "Binance Agentic Wallet live tests require Linux")
        endif()
        list(APPEND EXCHANGE_TEST_SOURCES
            tests/binance/binance_agentic_wallet_live_test.cpp)
    endif()

    add_executable(exchange_test ${EXCHANGE_TEST_SOURCES})
    target_link_libraries(exchange_test
        PRIVATE
            exchange::core
            exchange::binance_agent_os
            exchange::model
            exchange::hackathon_demo
            exchange::protocol
            GTest::gtest_main
            Threads::Threads
    )
    if(TARGET exchange_gateway)
        target_link_libraries(exchange_test PRIVATE exchange::gateway)
    endif()
    if(TARGET exchange_x402)
        target_link_libraries(exchange_test PRIVATE exchange::x402)
    endif()
    if(TARGET exchange_binance_agentic_wallet)
        target_link_libraries(exchange_test
            PRIVATE exchange::binance_agentic_wallet)
    endif()
    target_compile_features(exchange_test PRIVATE cxx_std_20)

    include(GoogleTest)
    gtest_discover_tests(exchange_test)
endif()
