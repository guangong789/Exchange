#include "gateway/tcp_gateway.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    struct TcpGatewayTestAccess {
        static bool enqueue_unexpected_protocol_error(TcpGateway& gateway) {
            return gateway.command_queue_.try_push(
                TcpGateway::CommandEnvelope{
                    0,
                    ProtocolError{
                        static_cast<ProtocolErrorCode>(-1)}});
        }

        static void stop_worker(TcpGateway& gateway) {
            gateway.request_stop();
            if (gateway.execution_thread_.joinable()) {
                gateway.execution_thread_.join();
            }
        }

        static const TradingRuntime& runtime(const TcpGateway& gateway) {
            return *gateway.runtime_;
        }
    };

    namespace {
        constexpr InstrumentContext kTestInstrument{20, 10, 1, 1, 1};
        constexpr Amount kInitialBalance = 1'000'000;

        std::unique_ptr<TradingRuntime> make_test_runtime() {
            auto runtime =
                std::make_unique<TradingRuntime>(kTestInstrument);
            for (const AccountId account_id : {1U, 2U}) {
                if (!runtime->accounts().create_account(account_id)) {
                    throw std::logic_error("duplicate test account");
                }
                runtime->accounts().fund(
                    account_id,
                    kTestInstrument.base_asset,
                    kInitialBalance);
                runtime->accounts().fund(
                    account_id,
                    kTestInstrument.quote_asset,
                    kInitialBalance);
            }
            if (!runtime->accounts().create_account(3)) {
                throw std::logic_error("duplicate unfunded test account");
            }
            return runtime;
        }

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

            void reset_with_rst() {
                const linger reset_linger{1, 0};
                if (::setsockopt(
                        fd_,
                        SOL_SOCKET,
                        SO_LINGER,
                        &reset_linger,
                        sizeof(reset_linger)) == -1) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "test setsockopt SO_LINGER");
                }
                reset();
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

        GatewayClient connect_gateway_client(TcpGateway& gateway) {
            const std::size_t expected_connections =
                gateway.connection_count() + 1;
            GatewayClient client = connect_gateway(gateway.local_port());
            poll_gateway_until(gateway, [&] {
                return gateway.connection_count() == expected_connections;
            });
            return client;
        }

        class TcpGatewayTest : public ::testing::Test {
        protected:
            GatewayClient connect_client() {
                return connect_gateway_client(gateway_);
            }

            TcpGateway gateway_{0, make_test_runtime()};
        };

        TEST_F(TcpGatewayTest, ValidAddResponseIsReceivedOverTcp) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(), "ADD 101 1 BUY 100 5\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n");

            TcpGatewayTestAccess::stop_worker(gateway_);
            const TradingRuntime& runtime =
                TcpGatewayTestAccess::runtime(gateway_);
            EXPECT_EQ(
                runtime.accounts().find_balance(1, 10),
                (Balance{kInitialBalance - 500, 500}));
            EXPECT_EQ(
                runtime.reservations().find(1),
                (OrderReservation{1, 10, 500, 500}));
            EXPECT_TRUE(runtime.order_book().find_order(1).has_value());
            EXPECT_EQ(runtime.ledger().entries().size(), 1U);
        }

        TEST_F(TcpGatewayTest, ValidCancelResponseIsReceivedOverTcp) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(), "ADD 101 1 BUY 100 5\n");
            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n");

            send_gateway_bytes(client.get(), "CANCEL 102 1 1\n");
            receive_exact(
                gateway_,
                client.get(),
                "RESULT 102 CANCELLED 0 1\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 5 1\n");

            TcpGatewayTestAccess::stop_worker(gateway_);
            const TradingRuntime& runtime =
                TcpGatewayTestAccess::runtime(gateway_);
            EXPECT_EQ(
                runtime.accounts().find_balance(1, 10),
                (Balance{kInitialBalance, 0}));
            EXPECT_FALSE(runtime.reservations().find(1).has_value());
            EXPECT_EQ(runtime.order_book().order_count(), 0U);
            EXPECT_EQ(runtime.ledger().entries().size(), 2U);
        }

        TEST_F(TcpGatewayTest, OldAndMalformedWireGrammarAreRejected) {
            const GatewayClient client = connect_client();

            send_gateway_bytes(
                client.get(),
                "BROKEN\n"
                "ADD 1 BUY 0 5 10\n"
                "ADD 1 1 99 BUY 0 5 10\n");

            receive_exact(
                gateway_,
                client.get(),
                "ERR MALFORMED_COMMAND\n"
                "ERR MALFORMED_COMMAND\n"
                "ERR MALFORMED_COMMAND\n");
        }

        TEST_F(TcpGatewayTest,
               AccountBackedTradePreservesEventsAndFinancialState) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(), "ADD 101 1 SELL 100 5\n");
            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 100 5 1\n");

            send_gateway_bytes(
                client.get(), "ADD 202 2 BUY 100 5\n");
            receive_exact(
                gateway_,
                client.get(),
                "RESULT 202 ACCEPTED 2 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 2\n"
                "EVENT TRADE_CREATED 2 1 100 5 2\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");

            TcpGatewayTestAccess::stop_worker(gateway_);
            const TradingRuntime& runtime =
                TcpGatewayTestAccess::runtime(gateway_);
            EXPECT_EQ(
                runtime.accounts().find_balance(1, 20),
                (Balance{kInitialBalance - 5, 0}));
            EXPECT_EQ(
                runtime.accounts().find_balance(1, 10),
                (Balance{kInitialBalance + 500, 0}));
            EXPECT_EQ(
                runtime.accounts().find_balance(2, 20),
                (Balance{kInitialBalance + 5, 0}));
            EXPECT_EQ(
                runtime.accounts().find_balance(2, 10),
                (Balance{kInitialBalance - 500, 0}));
            EXPECT_FALSE(runtime.reservations().find(1).has_value());
            EXPECT_FALSE(runtime.reservations().find(2).has_value());
            EXPECT_EQ(runtime.order_book().order_count(), 0U);
            EXPECT_EQ(runtime.ledger().entries().size(), 3U);
        }

        TEST_F(TcpGatewayTest,
               BusinessFailuresReturnResultsAndWorkerContinues) {
            const GatewayClient client = connect_client();

            send_gateway_bytes(
                client.get(),
                "ADD 301 3 BUY 100 1\n"
                "ADD 302 999 BUY 100 1\n"
                "ADD 303 1 BUY 100 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 301 INSUFFICIENT_FUNDS 0 0\n"
                "RESULT 302 ACCOUNT_NOT_FOUND 0 0\n"
                "RESULT 303 ACCEPTED 3 1\n"
                "EVENT ORDER_ACCEPTED 3 BUY 100 1 3\n");
        }

        TEST_F(TcpGatewayTest, NonOwnerCancelIsRejectedAndOwnerCanCancel) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(),
                "ADD 401 1 SELL 100 2\n"
                "CANCEL 402 2 1\n"
                "CANCEL 403 1 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 401 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 100 2 1\n"
                "RESULT 402 CANCEL_NOT_OWNER 0 0\n"
                "RESULT 403 CANCELLED 0 1\n"
                "EVENT ORDER_CANCELLED 1 SELL 100 2 1\n");
        }

        TEST_F(TcpGatewayTest,
               RepeatedRequestIdAllocatesDistinctOrdersWithoutDeduplication) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(),
                "ADD 501 1 BUY 100 1\n"
                "ADD 501 1 BUY 99 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 501 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 1 1\n"
                "RESULT 501 ACCEPTED 2 1\n"
                "EVENT ORDER_ACCEPTED 2 BUY 99 1 2\n");
        }

        TEST_F(TcpGatewayTest, MultipleCommandsPreserveResponseByteOrder) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(),
                "ADD 101 1 BUY 100 5\n"
                "CANCEL 102 1 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n"
                "RESULT 102 CANCELLED 0 1\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 5 1\n");
        }

        TEST_F(TcpGatewayTest, MalformedCommandPreservesResponseOrder) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(),
                "ADD 101 1 BUY 100 5\n"
                "BROKEN\n"
                "CANCEL 102 1 1\n");

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n"
                "ERR MALFORMED_COMMAND\n"
                "RESULT 102 CANCELLED 0 1\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 5 1\n");
        }

        TEST_F(TcpGatewayTest, TwoClientsReceiveOnlyTheirOwnResponses) {
            const GatewayClient first_client = connect_client();
            const GatewayClient second_client = connect_client();

            send_gateway_bytes(
                first_client.get(), "ADD 101 1 SELL 100 5\n");
            receive_exact(
                gateway_,
                first_client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 100 5 1\n");

            std::string unexpected_first_response;
            EXPECT_FALSE(receive_available(
                second_client.get(), unexpected_first_response));
            EXPECT_TRUE(unexpected_first_response.empty());

            send_gateway_bytes(
                second_client.get(), "ADD 202 2 BUY 100 5\n");
            receive_exact(
                gateway_,
                second_client.get(),
                "RESULT 202 ACCEPTED 2 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 2\n"
                "EVENT TRADE_CREATED 2 1 100 5 2\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");

            std::string unexpected_second_response;
            EXPECT_FALSE(receive_available(
                first_client.get(), unexpected_second_response));
            EXPECT_TRUE(unexpected_second_response.empty());
        }

        TEST_F(TcpGatewayTest, SplitCommandReceivesOneCompleteResponse) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(client.get(), "ADD 101 1 BUY 100");
            gateway_.poll_once(20);

            std::string premature_response;
            EXPECT_FALSE(receive_available(client.get(), premature_response));
            EXPECT_TRUE(premature_response.empty());

            send_gateway_bytes(client.get(), " 5\n");
            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n");
        }

        TEST_F(TcpGatewayTest, HalfCloseFlushesResponseBeforeServerCloses) {
            const GatewayClient client = connect_client();
            send_gateway_bytes(
                client.get(), "ADD 101 1 BUY 100 5\n");
            client.shutdown_write();

            receive_exact(
                gateway_,
                client.get(),
                "RESULT 101 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 5 1\n");
            poll_gateway_until(gateway_, [&] {
                return gateway_.connection_count() == 0;
            });

            EXPECT_EQ(gateway_.connection_count(), 0);
        }

        TEST_F(TcpGatewayTest, DisconnectDoesNotRollbackExecutedCommand) {
            GatewayClient first_client = connect_client();
            const GatewayClient second_client = connect_client();

            send_gateway_bytes(
                first_client.get(), "ADD 101 1 SELL 100 5\n");
            first_client.reset();
            poll_gateway_until(gateway_, [&] {
                return gateway_.connection_count() == 1;
            });

            send_gateway_bytes(
                second_client.get(), "ADD 202 2 BUY 100 5\n");
            receive_exact(
                gateway_,
                second_client.get(),
                "RESULT 202 ACCEPTED 2 4\n"
                "EVENT ORDER_ACCEPTED 2 BUY 100 5 2\n"
                "EVENT TRADE_CREATED 2 1 100 5 2\n"
                "EVENT ORDER_FILLED 1 SELL 5\n"
                "EVENT ORDER_FILLED 2 BUY 5\n");
        }

        TEST(TcpGatewayBackpressureTest, FullResponseQueueResumesAfterIoDrain) {
            using namespace std::chrono_literals;

            TcpGateway gateway{0, make_test_runtime(), 8, 1};
            const GatewayClient client = connect_gateway_client(gateway);
            send_gateway_bytes(
                client.get(),
                "ADD 110 1 BUY 90 1\n"
                "ADD 111 1 BUY 89 1\n"
                "ADD 112 1 BUY 88 1\n");

            gateway.poll_once(100);
            std::this_thread::sleep_for(25ms);

            receive_exact(
                gateway,
                client.get(),
                "RESULT 110 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 90 1 1\n"
                "RESULT 111 ACCEPTED 2 1\n"
                "EVENT ORDER_ACCEPTED 2 BUY 89 1 2\n"
                "RESULT 112 ACCEPTED 3 1\n"
                "EVENT ORDER_ACCEPTED 3 BUY 88 1 3\n");
        }

        TEST(TcpGatewayBackpressureTest, FullCommandQueueClosesOnlyOffender) {
            TcpGateway gateway{0, make_test_runtime(), 1, 1};
            const GatewayClient offending_client =
                connect_gateway_client(gateway);
            const GatewayClient healthy_client =
                connect_gateway_client(gateway);

            send_gateway_bytes(
                offending_client.get(),
                "BROKEN\n"
                "BROKEN\n"
                "BROKEN\n"
                "BROKEN\n");
            poll_gateway_until(gateway, [&] {
                return gateway.connection_count() == 1;
            });

            std::string healthy_bytes;
            EXPECT_FALSE(receive_available(
                healthy_client.get(), healthy_bytes));
            EXPECT_TRUE(healthy_bytes.empty());
            EXPECT_EQ(gateway.connection_count(), 1);
        }

        TEST(TcpGatewayBackpressureTest,
             DisconnectedClientResponsesAreNotRoutedToReplacement) {
            using namespace std::chrono_literals;

            TcpGateway gateway{0, make_test_runtime(), 8, 1};
            GatewayClient disconnected_client =
                connect_gateway_client(gateway);
            send_gateway_bytes(
                disconnected_client.get(),
                "BROKEN\nBROKEN\nBROKEN\n");
            gateway.poll_once(100);
            std::this_thread::sleep_for(25ms);

            disconnected_client.reset_with_rst();
            poll_gateway_until(gateway, [&] {
                return gateway.connection_count() == 0;
            });

            const GatewayClient replacement_client =
                connect_gateway_client(gateway);
            send_gateway_bytes(
                replacement_client.get(),
                "ADD 1200 1 SELL 110 1\n");
            receive_exact(
                gateway,
                replacement_client.get(),
                "RESULT 1200 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 SELL 110 1 1\n");

            std::string unexpected_response;
            EXPECT_FALSE(receive_available(
                replacement_client.get(), unexpected_response));
            EXPECT_TRUE(unexpected_response.empty());
        }

        TEST(TcpGatewayBackpressureTest,
             OutputLimitedClientDoesNotBlockHealthyClient) {
            using namespace std::chrono_literals;

            TcpGateway gateway{0, make_test_runtime(), 5'000, 5'000};
            const GatewayClient slow_client =
                connect_gateway_client(gateway);
            const GatewayClient healthy_client =
                connect_gateway_client(gateway);

            std::string slow_requests;
            slow_requests.reserve(4'000 * 7);
            for (int index = 0; index < 4'000; ++index) {
                slow_requests += "BROKEN\n";
            }
            send_gateway_bytes(slow_client.get(), slow_requests);
            gateway.poll_once(100);
            std::this_thread::sleep_for(25ms);

            send_gateway_bytes(
                healthy_client.get(),
                "ADD 601 1 BUY 100 1\n");
            receive_exact(
                gateway,
                healthy_client.get(),
                "RESULT 601 ACCEPTED 1 1\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 1 1\n");

            poll_gateway_until(gateway, [&] {
                return gateway.connection_count() == 1;
            });
        }

        TEST(TcpGatewayLifecycleTest, RequestStopWakesBlockingRun) {
            using namespace std::chrono_literals;

            TcpGateway gateway{0, make_test_runtime()};
            auto runner = std::async(
                std::launch::async,
                [&] { gateway.run(); });

            EXPECT_EQ(runner.wait_for(25ms), std::future_status::timeout);
            gateway.request_stop();

            ASSERT_EQ(runner.wait_for(1s), std::future_status::ready);
            EXPECT_NO_THROW(runner.get());
        }

        TEST(TcpGatewayLifecycleTest,
             RequestStopUnblocksWorkerWaitingOnFullResponseQueue) {
            using namespace std::chrono_literals;

            auto lifecycle = std::async(
                std::launch::async,
                [] {
                    TcpGateway gateway{0, make_test_runtime(), 8, 1};
                    const GatewayClient client =
                        connect_gateway_client(gateway);
                    send_gateway_bytes(
                        client.get(),
                        "BROKEN\nBROKEN\nBROKEN\n");
                    gateway.poll_once(100);
                    std::this_thread::sleep_for(25ms);
                    gateway.request_stop();
                });

            ASSERT_EQ(lifecycle.wait_for(1s), std::future_status::ready);
            EXPECT_NO_THROW(lifecycle.get());
        }

        TEST(TcpGatewayLifecycleTest,
             WorkerFailureIsRethrownFromRunInsteadOfTerminating) {
            using namespace std::chrono_literals;

            TcpGateway gateway{0, make_test_runtime()};
            ASSERT_TRUE(
                TcpGatewayTestAccess::enqueue_unexpected_protocol_error(
                    gateway));

            auto runner = std::async(
                std::launch::async,
                [&] { gateway.run(); });
            if (runner.wait_for(1s) != std::future_status::ready) {
                gateway.request_stop();
                FAIL() << "worker failure did not reach the I/O thread";
            }

            EXPECT_THROW(runner.get(), std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
