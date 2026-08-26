#include "exchange/epoll_server.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        class ScopedFd {
        public:
            explicit ScopedFd(int fd) : fd_(fd) {}

            ~ScopedFd() {
                reset();
            }

            ScopedFd(const ScopedFd&) = delete;
            ScopedFd& operator=(const ScopedFd&) = delete;

            ScopedFd(ScopedFd&& other) noexcept
                : fd_(std::exchange(other.fd_, -1)) {}

            ScopedFd& operator=(ScopedFd&& other) noexcept {
                if (this != &other) {
                    reset();
                    fd_ = std::exchange(other.fd_, -1);
                }
                return *this;
            }

            [[nodiscard]] int get() const noexcept {
                return fd_;
            }

            void reset() noexcept {
                if (fd_ != -1) {
                    ::close(fd_);
                    fd_ = -1;
                }
            }

        private:
            int fd_{-1};
        };

        struct ReceivedLine {
            int client_fd;
            std::string line;
        };

        ScopedFd connect_client(std::uint16_t port) {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "test socket");
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (::connect(
                    fd,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == -1) {
                const int error = errno;
                ::close(fd);
                throw std::system_error(
                    error, std::generic_category(), "test connect");
            }
            return ScopedFd{fd};
        }

        void send_all(int fd, std::string_view bytes) {
            std::size_t sent = 0;
            while (sent < bytes.size()) {
                const ssize_t result = ::send(
                    fd,
                    bytes.data() + sent,
                    bytes.size() - sent,
                    MSG_NOSIGNAL);
                if (result > 0) {
                    sent += static_cast<std::size_t>(result);
                    continue;
                }
                if (result == -1 && errno == EINTR) {
                    continue;
                }
                throw std::system_error(
                    errno, std::generic_category(), "test send");
            }
        }

        bool receive_available(int fd, std::string& received) {
            std::array<char, 4096> buffer{};
            while (true) {
                const ssize_t result = ::recv(
                    fd,
                    buffer.data(),
                    buffer.size(),
                    MSG_DONTWAIT);
                if (result > 0) {
                    received.append(
                        buffer.data(), static_cast<std::size_t>(result));
                    continue;
                }
                if (result == 0) {
                    return true;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return false;
                }
                throw std::system_error(
                    errno, std::generic_category(), "test recv");
            }
        }

        template <typename Predicate>
        void poll_until(EpollServer& server, Predicate predicate) {
            constexpr int kMaxPolls = 20;
            for (int attempt = 0; attempt < kMaxPolls; ++attempt) {
                if (std::invoke(predicate)) {
                    return;
                }
                server.poll_once(50);
            }
            ASSERT_TRUE(std::invoke(predicate));
        }

        TEST(EpollServerTest, PortZeroReturnsActualBoundPort) {
            EpollServer server(0, [](int, std::string_view) {});

            EXPECT_NE(server.local_port(), 0);
        }

        TEST(EpollServerTest, AcceptsOneClient) {
            EpollServer server(0, [](int, std::string_view) {});
            const ScopedFd client = connect_client(server.local_port());

            poll_until(server, [&] { return server.connection_count() == 1; });
            EXPECT_EQ(server.connection_count(), 1);
        }

        TEST(EpollServerTest, DeliversOneCompleteLine) {
            std::vector<ReceivedLine> lines;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view line) {
                    lines.push_back(ReceivedLine{client_fd, std::string{line}});
                });
            const ScopedFd client = connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 1; });

            send_all(client.get(), "ADD 1 BUY 100 5 10\n");
            poll_until(server, [&] { return lines.size() == 1; });

            EXPECT_EQ(lines[0].line, "ADD 1 BUY 100 5 10");
        }

        TEST(EpollServerTest, ReassemblesLineSplitAcrossTcpWrites) {
            std::vector<ReceivedLine> lines;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view line) {
                    lines.push_back(ReceivedLine{client_fd, std::string{line}});
                });
            const ScopedFd client = connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 1; });

            send_all(client.get(), "ADD 1 BUY 100");
            server.poll_once(100);
            EXPECT_TRUE(lines.empty());

            send_all(client.get(), " 5 10\n");
            poll_until(server, [&] { return lines.size() == 1; });
            EXPECT_EQ(lines[0].line, "ADD 1 BUY 100 5 10");
        }

        TEST(EpollServerTest, DeliversMultipleLinesFromOneTcpWrite) {
            std::vector<ReceivedLine> lines;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view line) {
                    lines.push_back(ReceivedLine{client_fd, std::string{line}});
                });
            const ScopedFd client = connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 1; });

            send_all(client.get(), "CANCEL 1\nCANCEL 2\n");
            poll_until(server, [&] { return lines.size() == 2; });

            EXPECT_EQ(lines[0].line, "CANCEL 1");
            EXPECT_EQ(lines[1].line, "CANCEL 2");
            EXPECT_EQ(lines[0].client_fd, lines[1].client_fd);
        }

        TEST(EpollServerTest, MaintainsIndependentFramingForTwoClients) {
            std::vector<ReceivedLine> lines;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view line) {
                    lines.push_back(ReceivedLine{client_fd, std::string{line}});
                });
            const ScopedFd first_client = connect_client(server.local_port());
            const ScopedFd second_client = connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 2; });

            send_all(first_client.get(), "ADD 1 BUY");
            send_all(second_client.get(), "CANCEL 2\n");
            poll_until(server, [&] { return lines.size() == 1; });
            EXPECT_EQ(lines[0].line, "CANCEL 2");

            send_all(first_client.get(), " 100 5 10\n");
            poll_until(server, [&] { return lines.size() == 2; });
            EXPECT_EQ(lines[1].line, "ADD 1 BUY 100 5 10");
            EXPECT_NE(lines[0].client_fd, lines[1].client_fd);
        }

        TEST(EpollServerTest, DisconnectDrainsDataAndRemovesConnectionState) {
            std::vector<std::string> lines;
            EpollServer server(
                0,
                [&](int, std::string_view line) {
                    lines.emplace_back(line);
                });
            ScopedFd client = connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 1; });

            send_all(client.get(), "CANCEL 7\n");
            client.reset();
            poll_until(server, [&] { return server.connection_count() == 0; });

            ASSERT_EQ(lines.size(), 1);
            EXPECT_EQ(lines[0], "CANCEL 7");
            EXPECT_EQ(server.connection_count(), 0);
        }

        TEST(EpollServerTest, PreservesPendingBytesAcrossPartialWrites) {
            const std::string payload(kMaxPendingOutputBytes, 'x');
            EpollServer* server_pointer = nullptr;
            int server_client_fd = -1;
            bool response_queued = false;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view) {
                    const int small_send_buffer = 1024;
                    if (::setsockopt(
                            client_fd,
                            SOL_SOCKET,
                            SO_SNDBUF,
                            &small_send_buffer,
                            sizeof(small_send_buffer)) == -1) {
                        throw std::system_error(
                            errno,
                            std::generic_category(),
                            "test setsockopt SO_SNDBUF");
                    }

                    server_client_fd = client_fd;
                    server_pointer->queue_write(client_fd, payload);
                    response_queued = true;
                });
            server_pointer = &server;

            const ScopedFd client = connect_client(server.local_port());
            const int small_receive_buffer = 1024;
            ASSERT_EQ(
                ::setsockopt(
                    client.get(),
                    SOL_SOCKET,
                    SO_RCVBUF,
                    &small_receive_buffer,
                    sizeof(small_receive_buffer)),
                0);
            poll_until(server, [&] { return server.connection_count() == 1; });

            send_all(client.get(), "WRITE\n");
            poll_until(server, [&] { return response_queued; });
            ASSERT_NE(server_client_fd, -1);

            server.poll_once(100);
            std::string received;
            static_cast<void>(receive_available(client.get(), received));
            EXPECT_LT(received.size(), payload.size());

            constexpr int kMaxWritePolls = 500;
            for (int attempt = 0;
                 attempt < kMaxWritePolls && received.size() < payload.size();
                 ++attempt) {
                server.poll_once(20);
                static_cast<void>(receive_available(client.get(), received));
            }

            EXPECT_EQ(received, payload);
            EXPECT_EQ(server.connection_count(), 1);
        }

        TEST(EpollServerTest, OutputLimitStopsBufferedLinesAndClosesOnlyClient) {
            EpollServer* server_pointer = nullptr;
            int skipped_line_count = 0;
            EpollServer server(
                0,
                [&](int client_fd, std::string_view line) {
                    if (line == "FILL_LIMIT") {
                        server_pointer->queue_write(
                            client_fd,
                            std::string(kMaxPendingOutputBytes, 'x'));
                        return;
                    }
                    if (line == "OVERFLOW") {
                        server_pointer->queue_write(client_fd, "x");
                        return;
                    }
                    if (line == "SHOULD_NOT_RUN") {
                        ++skipped_line_count;
                        return;
                    }
                    if (line == "HEALTHY") {
                        server_pointer->queue_write(client_fd, "HEALTHY\n");
                    }
                });
            server_pointer = &server;

            const ScopedFd offending_client =
                connect_client(server.local_port());
            const ScopedFd healthy_client =
                connect_client(server.local_port());
            poll_until(server, [&] { return server.connection_count() == 2; });

            send_all(
                offending_client.get(),
                "FILL_LIMIT\nOVERFLOW\nSHOULD_NOT_RUN\n");
            poll_until(server, [&] { return server.connection_count() == 1; });

            EXPECT_EQ(skipped_line_count, 0);
            send_all(healthy_client.get(), "HEALTHY\n");

            std::string received;
            poll_until(server, [&] {
                static_cast<void>(
                    receive_available(healthy_client.get(), received));
                return received == "HEALTHY\n";
            });

            EXPECT_EQ(received, "HEALTHY\n");
            EXPECT_EQ(server.connection_count(), 1);
        }
    }  // namespace
}  // namespace exchange
