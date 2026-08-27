#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "exchange/line_protocol.hpp"

namespace exchange {
    inline constexpr std::size_t kMaxPendingOutputBytes = 64 * 1024;
    using ConnectionId = std::uint64_t;

    class EpollServer {
    public:
        // The line view is valid only for the duration of the callback.
        using LineHandler = std::function<void(
            ConnectionId, int, std::string_view)>;
        using WakeHandler = std::function<void()>;

        EpollServer(
            std::uint16_t port,
            LineHandler line_handler,
            WakeHandler wake_handler = {});
        ~EpollServer();

        EpollServer(const EpollServer&) = delete;
        EpollServer& operator=(const EpollServer&) = delete;
        EpollServer(EpollServer&&) = delete;
        EpollServer& operator=(EpollServer&&) = delete;

        void run();
        void poll_once(int timeout_ms);
        void queue_write(ConnectionId connection_id, std::string response);

        // These request-count methods must only be called on the I/O thread.
        [[nodiscard]] bool mark_request_in_flight(
            ConnectionId connection_id);
        [[nodiscard]] bool complete_request(ConnectionId connection_id);
        [[nodiscard]] bool request_close(ConnectionId connection_id);

        // This is the only method intended to be called from another thread.
        void notify() noexcept;

        [[nodiscard]] std::uint16_t local_port() const noexcept;
        [[nodiscard]] std::size_t connection_count() const noexcept;

    private:
        struct ConnectionState {
            int fd{-1};
            ConnectionId id{};
            LineFramer framer;
            std::string write_buffer;
            std::size_t write_offset{};
            std::size_t in_flight_requests{};
            bool read_closed{};
            bool close_requested{};
        };

        void accept_connections();
        void read_connection(ConnectionState& connection);
        void write_connection(ConnectionState& connection);
        void update_interest(const ConnectionState& connection);
        void drain_wakeup();
        void request_close(ConnectionState& connection);
        void cleanup_deferred_connections() noexcept;
        [[nodiscard]] ConnectionState* find_connection(
            ConnectionId connection_id) noexcept;
        [[nodiscard]] static bool has_pending_output(
            const ConnectionState& connection) noexcept;
        [[nodiscard]] static bool ready_for_graceful_close(
            const ConnectionState& connection) noexcept;
        void close_connection(int fd) noexcept;

        int listen_fd_{-1};
        int epoll_fd_{-1};
        int event_fd_{-1};
        std::uint16_t local_port_{};
        ConnectionId next_connection_id_{1};
        LineHandler line_handler_;
        WakeHandler wake_handler_;
        std::unordered_map<int, ConnectionState> connections_;
        std::unordered_map<ConnectionId, int> connection_fds_;
        std::vector<ConnectionId> deferred_closes_;
    };
}  // namespace exchange
