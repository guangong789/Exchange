#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include "exchange/bounded_queue.hpp"
#include "exchange/epoll_server.hpp"
#include "exchange/event_collector.hpp"
#include "exchange/matching_engine.hpp"

namespace exchange {
    inline constexpr std::size_t kDefaultCommandQueueCapacity = 1024;
    inline constexpr std::size_t kDefaultResponseQueueCapacity = 1024;

    class TcpGateway {
    public:
        explicit TcpGateway(
            std::uint16_t port,
            std::size_t command_queue_capacity =
                kDefaultCommandQueueCapacity,
            std::size_t response_queue_capacity =
                kDefaultResponseQueueCapacity);
        ~TcpGateway();

        TcpGateway(const TcpGateway&) = delete;
        TcpGateway& operator=(const TcpGateway&) = delete;
        TcpGateway(TcpGateway&&) = delete;
        TcpGateway& operator=(TcpGateway&&) = delete;

        void run();
        void poll_once(int timeout_ms);
        void request_stop() noexcept;

        [[nodiscard]] std::uint16_t local_port() const noexcept;
        [[nodiscard]] std::size_t connection_count() const noexcept;

    private:
        friend struct TcpGatewayTestAccess;

        struct CommandEnvelope {
            ConnectionId connection_id;
            CommandParseResult request;
        };

        struct ResponseEnvelope {
            ConnectionId connection_id;
            std::string encoded_response;
        };

        void handle_line(ConnectionId connection_id, std::string_view line);
        void handle_wakeup();
        void matching_loop() noexcept;
        void rethrow_worker_failure() const;
        [[nodiscard]] static std::string execute_request(
            const CommandParseResult& request,
            MatchingEngine& matching_engine,
            EventCollector& event_collector);
        [[nodiscard]] static std::string execute_command(
            const Command& command,
            MatchingEngine& matching_engine,
            EventCollector& event_collector);

        BoundedQueue<CommandEnvelope> command_queue_;
        BoundedQueue<ResponseEnvelope> response_queue_;
        std::atomic_bool stop_requested_{};
        std::exception_ptr worker_failure_;
        std::atomic_bool worker_failure_ready_{};
        EpollServer server_;
        std::thread matching_thread_;
    };
}  // namespace exchange
