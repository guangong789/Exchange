#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "exchange/line_protocol.hpp"

namespace exchange {
    inline constexpr std::size_t kMaxPendingOutputBytes = 64 * 1024;

    class EpollServer {
    public:
        // The line view is valid only for the duration of the callback.
        using LineHandler = std::function<void(int, std::string_view)>;

        EpollServer(std::uint16_t port, LineHandler line_handler);
        ~EpollServer();

        EpollServer(const EpollServer&) = delete;
        EpollServer& operator=(const EpollServer&) = delete;
        EpollServer(EpollServer&&) = delete;
        EpollServer& operator=(EpollServer&&) = delete;

        void run();
        void poll_once(int timeout_ms);
        void queue_write(int client_fd, std::string response);

        [[nodiscard]] std::uint16_t local_port() const noexcept;
        [[nodiscard]] std::size_t connection_count() const noexcept;

    private:
        struct ConnectionState {
            int fd{-1};
            LineFramer framer;
            std::string write_buffer;
            std::size_t write_offset{};
            bool read_closed{};
            bool close_requested{};
        };

        void accept_connections();
        void read_connection(ConnectionState& connection);
        void write_connection(ConnectionState& connection);
        void update_interest(const ConnectionState& connection);
        [[nodiscard]] static bool has_pending_output(
            const ConnectionState& connection) noexcept;
        void close_connection(int fd) noexcept;

        int listen_fd_{-1};
        int epoll_fd_{-1};
        std::uint16_t local_port_{};
        LineHandler line_handler_;
        std::unordered_map<int, ConnectionState> connections_;
    };
}  // namespace exchange
