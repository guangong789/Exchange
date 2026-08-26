#include "exchange/tcp_gateway.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        class GatewayClient {
        public:
            explicit GatewayClient(int fd) : fd_(fd) {}

            ~GatewayClient() {
                reset();
            }

            GatewayClient(const GatewayClient&) = delete;
            GatewayClient& operator=(const GatewayClient&) = delete;

            GatewayClient(GatewayClient&& other) noexcept
                : fd_(std::exchange(other.fd_, -1)) {}

            GatewayClient& operator=(GatewayClient&&) = delete;

            [[nodiscard]] int get() const noexcept {
                return fd_;
            }

            void shutdown_write() const {
                if (::shutdown(fd_, SHUT_WR) == -1) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "test shutdown write");
                }
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

        GatewayClient connect_gateway(std::uint16_t port) {
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
            return GatewayClient{fd};
        }

        void send_gateway_bytes(int fd, std::string_view bytes) {
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
        void poll_gateway_until(TcpGateway& gateway, Predicate predicate) {
            constexpr int kMaxPolls = 100;
            for (int attempt = 0; attempt < kMaxPolls; ++attempt) {
                if (std::invoke(predicate)) {
                    return;
                }
                gateway.poll_once(20);
            }
            ASSERT_TRUE(std::invoke(predicate));
        }

        std::string receive_exact(
            TcpGateway& gateway,
            int client_fd,
            std::string_view expected) {
            std::string received;
            bool peer_closed = false;
            poll_gateway_until(gateway, [&] {
                peer_closed = receive_available(client_fd, received);
                return received.size() >= expected.size() || peer_closed;
            });

            EXPECT_EQ(received, expected);
            return received;
        }

        class TcpGatewayTest : public ::testing::Test {
        protected:
            GatewayClient connect_client() {
                const std::size_t expected_connections =
                    gateway_.connection_count() + 1;
                GatewayClient client = connect_gateway(gateway_.local_port());
                poll_gateway_until(gateway_, [&] {
                    return gateway_.connection_count() == expected_connections;
                });
                return client;
            }

            TcpGateway gateway_{0};
        };

        TEST_F(TcpGatewayTest, ValidAddResponseIsReceivedOverTcp) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 1 BUY 100 5 10\n");

            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 10\n");
        }

        TEST_F(TcpGatewayTest, ValidCancelResponseIsReceivedOverTcp) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 1 BUY 100 5 10\n");
            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 10\n");

            send_gateway_bytes(client.get(), "CANCEL 1\n");
            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 5 10\n");
        }

        TEST_F(TcpGatewayTest, ProtocolErrorsAreReceivedOverTcp) {
            const GatewayClient client = connect_client();

            send_gateway_bytes(
                client.get(),
                "BROKEN\n"
                "ADD 1 BUY 0 5 10\n"
                "CANCEL 99\n");

            receive_exact(
                gateway_,
                client.get(),
                "ERR MALFORMED_COMMAND\n"
                "ERR INVALID_ORDER\n"
                "ERR CANCEL_NOT_FOUND 99\n");
        }

        TEST_F(TcpGatewayTest, MatchingResponseContainsOrderedEventLines) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 1 SELL 100 5 10\n");
            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 100 5 10\n");

            send_gateway_bytes(client.get(), "ADD 2 BUY 100 5 20\n");
            receive_exact(
                gateway_,
                client.get(),
                "OK 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 20\n"
                "EVENT TRADE_CREATED 2 1 100 5 20\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");
        }

        TEST_F(TcpGatewayTest, MultipleCommandsPreserveResponseByteOrder) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(),
                "ADD 1 BUY 100 5 10\n"
                "CANCEL 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 10\n"
                "OK 1\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 5 10\n");
        }

        TEST_F(TcpGatewayTest, TwoClientsReceiveOnlyTheirOwnResponses) {
            const GatewayClient first_client = connect_client();
            const GatewayClient second_client = connect_client();

            send_gateway_bytes(
                first_client.get(), "ADD 1 SELL 100 5 10\n");
            receive_exact(
                gateway_,
                first_client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 100 5 10\n");

            std::string unexpected_first_response;
            EXPECT_FALSE(receive_available(
                second_client.get(), unexpected_first_response));
            EXPECT_TRUE(unexpected_first_response.empty());

            send_gateway_bytes(
                second_client.get(), "ADD 2 BUY 100 5 20\n");
            receive_exact(
                gateway_,
                second_client.get(),
                "OK 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 20\n"
                "EVENT TRADE_CREATED 2 1 100 5 20\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");

            std::string unexpected_second_response;
            EXPECT_FALSE(receive_available(
                first_client.get(), unexpected_second_response));
            EXPECT_TRUE(unexpected_second_response.empty());
        }

        TEST_F(TcpGatewayTest, SplitCommandReceivesOneCompleteResponse) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 1 BUY 100");
            gateway_.poll_once(20);

            std::string premature_response;
            EXPECT_FALSE(receive_available(client.get(), premature_response));
            EXPECT_TRUE(premature_response.empty());

            send_gateway_bytes(client.get(), " 5 10\n");
            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 10\n");
        }

        TEST_F(TcpGatewayTest, HalfCloseFlushesResponseBeforeServerCloses) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 1 BUY 100 5 10\n");
            client.shutdown_write();

            receive_exact(
                gateway_,
                client.get(),
                "OK 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 10\n");
            poll_gateway_until(gateway_, [&] {
                return gateway_.connection_count() == 0;
            });

            EXPECT_EQ(gateway_.connection_count(), 0);
        }

        TEST_F(TcpGatewayTest, DisconnectDoesNotRollbackExecutedCommand) {
            GatewayClient first_client = connect_client();
            const GatewayClient second_client = connect_client();

            send_gateway_bytes(
                first_client.get(), "ADD 1 SELL 100 5 10\n");
            first_client.reset();
            poll_gateway_until(gateway_, [&] {
                return gateway_.connection_count() == 1;
            });

            send_gateway_bytes(
                second_client.get(), "ADD 2 BUY 100 5 20\n");
            receive_exact(
                gateway_,
                second_client.get(),
                "OK 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 20\n"
                "EVENT TRADE_CREATED 2 1 100 5 20\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");
        }
    }  // namespace
}  // namespace exchange
