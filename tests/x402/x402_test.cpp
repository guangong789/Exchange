#include "exchange/x402/x402.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "exchange/accounting/account_store.hpp"
#include "exchange/accounting/ledger.hpp"
#include "exchange/matching/order_book.hpp"

namespace exchange {
    namespace {
        class OneShotHttpResponseServer {
        public:
            explicit OneShotHttpResponseServer(std::string response)
                : response_(std::move(response)) {
                listen_fd_ = ::socket(
                    AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (listen_fd_ < 0) {
                    throw std::runtime_error("test socket failed");
                }
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = 0;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::bind(
                        listen_fd_,
                        reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)) < 0
                    || ::listen(listen_fd_, 1) < 0) {
                    const std::string detail = std::strerror(errno);
                    static_cast<void>(::close(listen_fd_));
                    throw std::runtime_error(
                        "test server setup failed: " + detail);
                }
                socklen_t length = sizeof(address);
                if (::getsockname(
                        listen_fd_,
                        reinterpret_cast<sockaddr*>(&address),
                        &length) < 0) {
                    const std::string detail = std::strerror(errno);
                    static_cast<void>(::close(listen_fd_));
                    throw std::runtime_error(
                        "test getsockname failed: " + detail);
                }
                port_ = ntohs(address.sin_port);
            }

            ~OneShotHttpResponseServer() {
                if (listen_fd_ >= 0) {
                    static_cast<void>(::close(listen_fd_));
                }
            }

            OneShotHttpResponseServer(const OneShotHttpResponseServer&) = delete;
            OneShotHttpResponseServer& operator=(
                const OneShotHttpResponseServer&) = delete;

            std::string url() const {
                return "http://127.0.0.1:" + std::to_string(port_) + "/test";
            }

            void serve_once() {
                int client;
                do {
                    client = ::accept4(
                        listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                } while (client < 0 && errno == EINTR);
                if (client < 0) {
                    throw std::runtime_error("test accept failed");
                }

                std::array<char, 2048> request{};
                ssize_t received;
                do {
                    received = ::recv(
                        client, request.data(), request.size(), 0);
                } while (received < 0 && errno == EINTR);
                if (received <= 0) {
                    static_cast<void>(::close(client));
                    throw std::runtime_error("test recv failed");
                }

                std::size_t sent = 0;
                while (sent < response_.size()) {
                    const ssize_t result = ::send(
                        client,
                        response_.data() + sent,
                        response_.size() - sent,
                        MSG_NOSIGNAL);
                    if (result > 0) {
                        sent += static_cast<std::size_t>(result);
                    } else if (result < 0 && errno == EINTR) {
                        continue;
                    } else {
                        static_cast<void>(::close(client));
                        throw std::runtime_error("test send failed");
                    }
                }
                static_cast<void>(::close(client));
            }

        private:
            std::string response_;
            int listen_fd_{-1};
            std::uint16_t port_{};
        };

        template <typename Server>
        std::future<void> serve_async(Server& server) {
            return std::async(std::launch::async, [&server] {
                server.serve_once();
            });
        }

        X402PaidServiceConfig test_config() {
            return X402PaidServiceConfig{
                "/premium-signal",
                "Premium market signal from Agent B",
                "application/json",
                "exact",
                "eip155:97",
                "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                "10000",
                "0x1111111111111111111111111111111111111111",
                60,
            };
        }

        TEST(X402IntegrationTest,
             AgentAToAgentBReceivesX402PaymentRequiredWithoutWorldMutation) {
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);
            const auto balance_before = accounts.find_balance(1, 10);

            OrderBook order_book;
            const auto trades = order_book.add_order(
                Order{1, Side::Sell, OrderType::Limit, 100, 2, 1});
            ASSERT_TRUE(trades.empty());
            const auto order_before = order_book.find_order(1);
            const std::size_t order_count_before = order_book.order_count();

            Ledger ledger;
            const auto ledger_before = ledger.entries();

            X402PaidService service(test_config());
            auto server = serve_async(service);
            CurlX402Client client;
            const X402RequestResult result = client.get(
                service.resource_url());
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());

            EXPECT_EQ(result.http_status, 402);
            EXPECT_FALSE(result.resource_body.has_value());
            ASSERT_TRUE(result.payment_required.has_value());
            EXPECT_EQ(
                *result.payment_required,
                (ExternalPaymentRequirement{
                    2,
                    service.resource_url(),
                    "Premium market signal from Agent B",
                    "application/json",
                    "exact",
                    "eip155:97",
                    "10000",
                    "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                    "0x1111111111111111111111111111111111111111",
                    60}));

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            const auto order_after = order_book.find_order(1);
            ASSERT_TRUE(order_before.has_value());
            ASSERT_TRUE(order_after.has_value());
            EXPECT_EQ(order_after->id, order_before->id);
            EXPECT_EQ(order_after->side, order_before->side);
            EXPECT_EQ(order_after->type, order_before->type);
            EXPECT_EQ(order_after->price, order_before->price);
            EXPECT_EQ(order_after->quantity, order_before->quantity);
            EXPECT_EQ(order_after->timestamp, order_before->timestamp);
            EXPECT_EQ(order_book.order_count(), order_count_before);
            EXPECT_EQ(order_book.best_ask(), 100);
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST(X402PaidServiceTest,
             BindsEphemeralLoopbackPortAndUsesAbsoluteResourceUrl) {
            X402PaidService service(test_config());

            EXPECT_NE(service.local_port(), 0);
            EXPECT_EQ(
                service.resource_url(),
                "http://127.0.0.1:"
                    + std::to_string(service.local_port())
                    + "/premium-signal");
        }

        TEST(X402ClientTest, Ordinary404IsNotAPaymentChallenge) {
            X402PaidService service(test_config());
            auto server = serve_async(service);
            CurlX402Client client;
            const std::string missing_url =
                "http://127.0.0.1:"
                + std::to_string(service.local_port()) + "/missing";

            const X402RequestResult result = client.get(missing_url);
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());

            EXPECT_EQ(result.http_status, 404);
            EXPECT_FALSE(result.payment_required.has_value());
            ASSERT_TRUE(result.resource_body.has_value());
            EXPECT_EQ(*result.resource_body, "Not Found\n");
        }

        TEST(X402ClientTest, RejectsIncompletePaymentRequiredMetadata) {
            OneShotHttpResponseServer service(
                "HTTP/1.1 402 Payment Required\r\n"
                "PAYMENT-REQUIRED: eyJ4NDAyVmVyc2lvbiI6Mn0=\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
            auto server = serve_async(service);
            CurlX402Client client;

            EXPECT_THROW(
                static_cast<void>(client.get(service.url())),
                X402ProtocolError);
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());
        }

        TEST(X402ClientTest, PreservesLargeAtomicAmountAsExactString) {
            auto config = test_config();
            config.amount = "123456789012345678901234567890";
            X402PaidService service(config);
            auto server = serve_async(service);
            CurlX402Client client;

            const X402RequestResult result = client.get(
                service.resource_url());
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());
            ASSERT_TRUE(result.payment_required.has_value());
            EXPECT_EQ(
                result.payment_required->amount,
                "123456789012345678901234567890");
        }

        TEST(X402ClientTest, Rejects402WithoutPaymentRequiredHeader) {
            OneShotHttpResponseServer service(
                "HTTP/1.1 402 Payment Required\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
            auto server = serve_async(service);
            CurlX402Client client;

            EXPECT_THROW(
                static_cast<void>(client.get(service.url())),
                X402ProtocolError);
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());
        }

        TEST(X402ClientTest, Non402BodyIsReturnedWithoutAutomaticRetry) {
            OneShotHttpResponseServer service(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n\r\nOK");
            auto server = serve_async(service);
            CurlX402Client client;

            const X402RequestResult result = client.get(service.url());
            ASSERT_EQ(server.wait_for(std::chrono::seconds(2)),
                      std::future_status::ready);
            ASSERT_NO_THROW(server.get());
            EXPECT_EQ(result.http_status, 200);
            EXPECT_FALSE(result.payment_required.has_value());
            EXPECT_EQ(result.resource_body, "OK");
        }

        TEST(X402ClientTest, RejectsNonLocalhostUrlsAndInvalidTimeout) {
            EXPECT_THROW(
                CurlX402Client(CurlX402ClientConfig{
                    std::chrono::milliseconds(0)}),
                std::invalid_argument);
            CurlX402Client client;
            EXPECT_THROW(
                static_cast<void>(client.get("https://example.com/paid")),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(client.get("http://localhost:9000/paid")),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(client.get("http://127.0.0.1:0/paid")),
                std::invalid_argument);
        }

        TEST(X402PaidServiceTest,
             RejectsInvalidRequirementBeforeOpeningService) {
            auto config = test_config();
            config.amount = "0.01";
            EXPECT_THROW(
                static_cast<void>(X402PaidService{config}),
                std::invalid_argument);
            config = test_config();
            config.network = "bsc-testnet";
            EXPECT_THROW(
                static_cast<void>(X402PaidService{config}),
                std::invalid_argument);
            config = test_config();
            config.resource_path = "premium-signal";
            EXPECT_THROW(
                static_cast<void>(X402PaidService{config}),
                std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
