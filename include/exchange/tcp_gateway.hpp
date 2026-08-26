#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "exchange/epoll_server.hpp"
#include "exchange/event_collector.hpp"
#include "exchange/matching_engine.hpp"

namespace exchange {
    class TcpGateway {
    public:
        explicit TcpGateway(std::uint16_t port);

        TcpGateway(const TcpGateway&) = delete;
        TcpGateway& operator=(const TcpGateway&) = delete;
        TcpGateway(TcpGateway&&) = delete;
        TcpGateway& operator=(TcpGateway&&) = delete;

        void run();
        void poll_once(int timeout_ms);

        [[nodiscard]] std::uint16_t local_port() const noexcept;
        [[nodiscard]] std::size_t connection_count() const noexcept;

    private:
        void handle_line(int client_fd, std::string_view line);
        [[nodiscard]] std::string execute_command(const Command& command);

        EventCollector event_collector_;
        MatchingEngine matching_engine_;
        EpollServer server_;
    };
}  // namespace exchange
