#include "exchange/gateway/epoll_server.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace exchange {
    namespace {
        constexpr int kMaxEventsPerPoll = 64;
        constexpr std::size_t kReadBufferSize = 4096;
    }  // namespace

    EpollServer::EpollServer(
        std::uint16_t port,
        LineHandler line_handler,
        WakeHandler wake_handler)
        : line_handler_(std::move(line_handler)),
          wake_handler_(std::move(wake_handler)) {
        if (!line_handler_) {
            throw std::invalid_argument("line handler must not be empty");
        }

        try {
            listen_fd_ = ::socket(
                AF_INET,
                SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                0);
            if (listen_fd_ == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "socket");
            }

            const int reuse_address = 1;
            if (::setsockopt(
                    listen_fd_,
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    &reuse_address,
                    sizeof(reuse_address)) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "setsockopt SO_REUSEADDR");
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (::bind(
                    listen_fd_,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "bind");
            }

            if (::listen(listen_fd_, SOMAXCONN) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "listen");
            }

            socklen_t address_length = sizeof(address);
            if (::getsockname(
                    listen_fd_,
                    reinterpret_cast<sockaddr*>(&address),
                    &address_length) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "getsockname");
            }
            local_port_ = ntohs(address.sin_port);

            epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
            if (epoll_fd_ == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "epoll_create1");
            }

            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = listen_fd_;
            if (::epoll_ctl(
                    epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "epoll_ctl add listening socket");
            }

            event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (event_fd_ == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "eventfd");
            }

            event = {};
            event.events = EPOLLIN;
            event.data.fd = event_fd_;
            if (::epoll_ctl(
                    epoll_fd_, EPOLL_CTL_ADD, event_fd_, &event) == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "epoll_ctl add eventfd");
            }
        } catch (...) {
            if (event_fd_ != -1) {
                ::close(event_fd_);
                event_fd_ = -1;
            }
            if (epoll_fd_ != -1) {
                ::close(epoll_fd_);
                epoll_fd_ = -1;
            }
            if (listen_fd_ != -1) {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            throw;
        }
    }

    EpollServer::~EpollServer() {
        for (const auto& entry : connections_) {
            ::close(entry.first);
        }
        connections_.clear();
        connection_fds_.clear();

        if (listen_fd_ != -1) {
            ::close(listen_fd_);
        }
        if (event_fd_ != -1) {
            ::close(event_fd_);
        }
        if (epoll_fd_ != -1) {
            ::close(epoll_fd_);
        }
    }

    void EpollServer::run() {
        while (true) {
            poll_once(-1);
        }
    }

    void EpollServer::poll_once(int timeout_ms) {
        if (timeout_ms < -1) {
            throw std::invalid_argument("epoll timeout must be -1 or non-negative");
        }

        cleanup_deferred_connections();

        std::array<epoll_event, kMaxEventsPerPoll> events{};
        int ready_count = -1;
        do {
            ready_count = ::epoll_wait(
                epoll_fd_,
                events.data(),
                static_cast<int>(events.size()),
                timeout_ms);
        } while (ready_count == -1 && errno == EINTR);

        if (ready_count == -1) {
            throw std::system_error(
                errno, std::generic_category(), "epoll_wait");
        }

        for (int index = 0; index < ready_count; ++index) {
            const int fd = events[static_cast<std::size_t>(index)].data.fd;
            const std::uint32_t flags =
                events[static_cast<std::size_t>(index)].events;

            if (fd == listen_fd_) {
                if ((flags & (EPOLLERR | EPOLLHUP)) != 0U) {
                    int socket_error = 0;
                    socklen_t error_length = sizeof(socket_error);
                    if (::getsockopt(
                            listen_fd_,
                            SOL_SOCKET,
                            SO_ERROR,
                            &socket_error,
                            &error_length) == -1) {
                        socket_error = errno;
                    }
                    if (socket_error == 0) {
                        socket_error = EIO;
                    }
                    throw std::system_error(
                        socket_error,
                        std::generic_category(),
                        "listening socket epoll error");
                }
                if ((flags & EPOLLIN) != 0U) {
                    accept_connections();
                }
                continue;
            }

            if (fd == event_fd_) {
                if ((flags & (EPOLLERR | EPOLLHUP)) != 0U) {
                    throw std::system_error(
                        EIO,
                        std::generic_category(),
                        "eventfd epoll error");
                }
                if ((flags & EPOLLIN) != 0U) {
                    drain_wakeup();
                    if (wake_handler_) {
                        wake_handler_();
                    }
                }
                continue;
            }

            const auto connection = connections_.find(fd);
            if (connection == connections_.end()) {
                continue;
            }

            ConnectionState& state = connection->second;
            if (!state.read_closed &&
                (flags & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0U) {
                read_connection(state);
            }

            if (state.close_requested ||
                (flags & (EPOLLERR | EPOLLHUP)) != 0U) {
                close_connection(fd);
                continue;
            }

            if ((flags & EPOLLRDHUP) != 0U) {
                state.read_closed = true;
            }

            if ((flags & EPOLLOUT) != 0U) {
                write_connection(state);
            }

            if (state.close_requested) {
                close_connection(fd);
                continue;
            }

            if (state.read_closed) {
                if (ready_for_graceful_close(state)) {
                    close_connection(fd);
                } else {
                    update_interest(state);
                }
                continue;
            }

            if ((flags & EPOLLOUT) != 0U &&
                !has_pending_output(state)) {
                update_interest(state);
            }
        }

        cleanup_deferred_connections();
    }

    void EpollServer::queue_write(
        ConnectionId connection_id,
        std::string response) {
        ConnectionState* const state = find_connection(connection_id);
        if (state == nullptr || response.empty()) {
            return;
        }

        if (state->close_requested) {
            return;
        }

        const std::size_t pending_bytes =
            state->write_buffer.size() - state->write_offset;
        if (response.size() > kMaxPendingOutputBytes - pending_bytes) {
            state->write_buffer.clear();
            state->write_offset = 0;
            request_close(*state);
            return;
        }

        const bool was_empty = pending_bytes == 0;
        if (state->write_offset != 0) {
            state->write_buffer.erase(0, state->write_offset);
            state->write_offset = 0;
        }
        state->write_buffer.append(response);

        if (was_empty) {
            update_interest(*state);
        }
    }

    bool EpollServer::mark_request_in_flight(ConnectionId connection_id) {
        ConnectionState* const connection = find_connection(connection_id);
        if (connection == nullptr || connection->close_requested) {
            return false;
        }
        if (connection->in_flight_requests ==
            std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("in-flight request count overflow");
        }

        ++connection->in_flight_requests;
        return true;
    }

    bool EpollServer::complete_request(ConnectionId connection_id) {
        ConnectionState* const connection = find_connection(connection_id);
        if (connection == nullptr) {
            return false;
        }
        if (connection->in_flight_requests == 0) {
            throw std::logic_error("no in-flight request to complete");
        }

        --connection->in_flight_requests;
        if (ready_for_graceful_close(*connection)) {
            request_close(*connection);
        }
        return true;
    }

    bool EpollServer::request_close(ConnectionId connection_id) {
        ConnectionState* const connection = find_connection(connection_id);
        if (connection == nullptr) {
            return false;
        }

        request_close(*connection);
        return true;
    }

    void EpollServer::notify() noexcept {
        const std::uint64_t increment = 1;
        while (true) {
            const ssize_t bytes_written =
                ::write(event_fd_, &increment, sizeof(increment));
            if (bytes_written == static_cast<ssize_t>(sizeof(increment))) {
                return;
            }
            if (bytes_written == -1 && errno == EINTR) {
                continue;
            }
            if (bytes_written == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            return;
        }
    }

    std::uint16_t EpollServer::local_port() const noexcept {
        return local_port_;
    }

    std::size_t EpollServer::connection_count() const noexcept {
        return connections_.size();
    }

    void EpollServer::accept_connections() {
        while (true) {
            const int client_fd = ::accept4(
                listen_fd_,
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == ECONNABORTED) {
                    continue;
                }
                throw std::system_error(
                    errno, std::generic_category(), "accept4");
            }

            if (next_connection_id_ == 0) {
                ::close(client_fd);
                continue;
            }

            const ConnectionId connection_id = next_connection_id_;
            if (next_connection_id_ ==
                std::numeric_limits<ConnectionId>::max()) {
                next_connection_id_ = 0;
            } else {
                ++next_connection_id_;
            }

            epoll_event event{};
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.fd = client_fd;
            if (::epoll_ctl(
                    epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                const int error = errno;
                ::close(client_fd);
                throw std::system_error(
                    error,
                    std::generic_category(),
                    "epoll_ctl add client socket");
            }

            try {
                const auto [connection, inserted] = connections_.emplace(
                    client_fd,
                    ConnectionState{
                        client_fd,
                        connection_id,
                        LineFramer{},
                        {},
                        0,
                        0,
                        false,
                        false});
                if (!inserted) {
                    throw std::logic_error("accepted duplicate client fd");
                }

                try {
                    const auto [id_entry, id_inserted] =
                        connection_fds_.emplace(connection_id, client_fd);
                    static_cast<void>(id_entry);
                    if (!id_inserted) {
                        throw std::logic_error(
                            "accepted duplicate connection id");
                    }
                } catch (...) {
                    connections_.erase(connection);
                    throw;
                }
            } catch (...) {
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
                ::close(client_fd);
                throw;
            }
        }
    }

    void EpollServer::read_connection(ConnectionState& connection) {
        std::array<char, kReadBufferSize> buffer{};

        while (!connection.close_requested && !connection.read_closed) {
            const ssize_t bytes_read = ::recv(
                connection.fd, buffer.data(), buffer.size(), 0);
            if (bytes_read > 0) {
                connection.framer.append(std::string_view{
                    buffer.data(), static_cast<std::size_t>(bytes_read)});

                while (true) {
                    LineFrameResult frame = connection.framer.next_line();
                    if (frame.status == LineFrameStatus::NeedMoreData) {
                        break;
                    }
                    if (frame.status == LineFrameStatus::LineTooLong) {
                        request_close(connection);
                        return;
                    }

                    line_handler_(connection.id, connection.fd, frame.line);
                    if (connection.close_requested) {
                        return;
                    }
                }
                continue;
            }

            if (bytes_read == 0) {
                connection.read_closed = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            request_close(connection);
            return;
        }
    }

    void EpollServer::write_connection(ConnectionState& connection) {
        while (has_pending_output(connection)) {
            const char* const pending_data =
                connection.write_buffer.data() + connection.write_offset;
            const std::size_t pending_size =
                connection.write_buffer.size() - connection.write_offset;
            const ssize_t bytes_written = ::send(
                connection.fd,
                pending_data,
                pending_size,
                MSG_NOSIGNAL);

            if (bytes_written > 0) {
                connection.write_offset +=
                    static_cast<std::size_t>(bytes_written);
                continue;
            }
            if (bytes_written == -1 && errno == EINTR) {
                continue;
            }
            if (bytes_written == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }

            request_close(connection);
            return;
        }

        connection.write_buffer.clear();
        connection.write_offset = 0;
    }

    void EpollServer::update_interest(const ConnectionState& connection) {
        epoll_event event{};
        if (!connection.read_closed) {
            event.events |= EPOLLIN | EPOLLRDHUP;
        }
        if (has_pending_output(connection)) {
            event.events |= EPOLLOUT;
        }
        event.data.fd = connection.fd;

        if (::epoll_ctl(
                epoll_fd_, EPOLL_CTL_MOD, connection.fd, &event) == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "epoll_ctl modify client socket");
        }
    }

    void EpollServer::drain_wakeup() {
        std::uint64_t value = 0;
        while (true) {
            const ssize_t bytes_read =
                ::read(event_fd_, &value, sizeof(value));
            if (bytes_read == static_cast<ssize_t>(sizeof(value))) {
                continue;
            }
            if (bytes_read == -1 && errno == EINTR) {
                continue;
            }
            if (bytes_read == -1 && errno == EAGAIN) {
                return;
            }
            throw std::system_error(
                bytes_read == -1 ? errno : EIO,
                std::generic_category(),
                "eventfd read");
        }
    }

    void EpollServer::request_close(ConnectionState& connection) {
        if (connection.close_requested) {
            return;
        }
        deferred_closes_.push_back(connection.id);
        connection.close_requested = true;
    }

    void EpollServer::cleanup_deferred_connections() noexcept {
        std::vector<ConnectionId> pending;
        pending.swap(deferred_closes_);

        for (const ConnectionId connection_id : pending) {
            ConnectionState* const connection = find_connection(connection_id);
            if (connection != nullptr && connection->close_requested) {
                close_connection(connection->fd);
            }
        }
    }

    EpollServer::ConnectionState* EpollServer::find_connection(
        ConnectionId connection_id) noexcept {
        const auto fd = connection_fds_.find(connection_id);
        if (fd == connection_fds_.end()) {
            return nullptr;
        }

        const auto connection = connections_.find(fd->second);
        if (connection == connections_.end() ||
            connection->second.id != connection_id) {
            return nullptr;
        }
        return &connection->second;
    }

    bool EpollServer::has_pending_output(
        const ConnectionState& connection) noexcept {
        return connection.write_offset < connection.write_buffer.size();
    }

    bool EpollServer::ready_for_graceful_close(
        const ConnectionState& connection) noexcept {
        return connection.read_closed &&
               connection.in_flight_requests == 0 &&
               !has_pending_output(connection);
    }

    void EpollServer::close_connection(int fd) noexcept {
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return;
        }

        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        connection_fds_.erase(connection->second.id);
        connections_.erase(connection);
    }
}  // namespace exchange
