#include "exchange/epoll_server.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace exchange {
    namespace {
        constexpr int kMaxEventsPerPoll = 64;
        constexpr std::size_t kReadBufferSize = 4096;
    }  // namespace

    EpollServer::EpollServer(std::uint16_t port, LineHandler line_handler)
        : line_handler_(std::move(line_handler)) {
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
        } catch (...) {
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

        if (listen_fd_ != -1) {
            ::close(listen_fd_);
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
                if (has_pending_output(state)) {
                    update_interest(state);
                } else {
                    close_connection(fd);
                }
                continue;
            }

            if ((flags & EPOLLOUT) != 0U &&
                !has_pending_output(state)) {
                update_interest(state);
            }
        }
    }

    void EpollServer::queue_write(int client_fd, std::string response) {
        const auto connection = connections_.find(client_fd);
        if (connection == connections_.end() || response.empty()) {
            return;
        }

        ConnectionState& state = connection->second;
        if (state.close_requested) {
            return;
        }

        const std::size_t pending_bytes =
            state.write_buffer.size() - state.write_offset;
        if (response.size() > kMaxPendingOutputBytes - pending_bytes) {
            state.write_buffer.clear();
            state.write_offset = 0;
            state.close_requested = true;
            return;
        }

        const bool was_empty = pending_bytes == 0;
        if (state.write_offset != 0) {
            state.write_buffer.erase(0, state.write_offset);
            state.write_offset = 0;
        }
        state.write_buffer.append(response);

        if (was_empty) {
            update_interest(state);
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
                        LineFramer{},
                        {},
                        0,
                        false,
                        false});
                static_cast<void>(connection);
                if (!inserted) {
                    throw std::logic_error("accepted duplicate client fd");
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
                        connection.close_requested = true;
                        return;
                    }

                    line_handler_(connection.fd, frame.line);
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
            connection.close_requested = true;
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

            connection.close_requested = true;
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

    bool EpollServer::has_pending_output(
        const ConnectionState& connection) noexcept {
        return connection.write_offset < connection.write_buffer.size();
    }

    void EpollServer::close_connection(int fd) noexcept {
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return;
        }

        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        connections_.erase(connection);
    }
}  // namespace exchange
