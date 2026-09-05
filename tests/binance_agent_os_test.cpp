#include "exchange/binance/binance_agent_os_client.hpp"

#include <limits>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "exchange/accounting/account_store.hpp"
#include "exchange/agent/agent_observation_service.hpp"
#include "exchange/agent/agent_registry.hpp"
#include "exchange/accounting/ledger.hpp"
#include "exchange/matching/order_book.hpp"

namespace exchange {
    namespace {
        class ScopedEnvironmentVariable final {
        public:
            ScopedEnvironmentVariable(
                const char* name,
                std::optional<std::string> value)
                : name_(name) {
                if (const char* existing = std::getenv(name_)) {
                    previous_value_ = existing;
                }
                if (value.has_value()) {
                    if (::setenv(name_, value->c_str(), 1) != 0) {
                        throw std::runtime_error("failed to set test environment");
                    }
                } else if (::unsetenv(name_) != 0) {
                    throw std::runtime_error("failed to unset test environment");
                }
            }

            ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
            ScopedEnvironmentVariable& operator=(
                const ScopedEnvironmentVariable&) = delete;

            ~ScopedEnvironmentVariable() {
                if (previous_value_.has_value()) {
                    static_cast<void>(::setenv(
                        name_, previous_value_->c_str(), 1));
                } else {
                    static_cast<void>(::unsetenv(name_));
                }
            }

        private:
            const char* name_;
            std::optional<std::string> previous_value_;
        };

        class LocalMcpServer final {
        public:
            LocalMcpServer() {
                listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (listen_fd_ < 0) {
                    throw std::runtime_error("failed to create test socket");
                }

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = 0;
                if (::bind(
                        listen_fd_,
                        reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address))
                        != 0
                    || ::listen(listen_fd_, 4) != 0) {
                    const int error = errno;
                    static_cast<void>(::close(listen_fd_));
                    listen_fd_ = -1;
                    throw std::runtime_error(
                        std::string("failed to bind test socket: ")
                        + std::strerror(error));
                }

                socklen_t size = sizeof(address);
                if (::getsockname(
                        listen_fd_,
                        reinterpret_cast<sockaddr*>(&address),
                        &size)
                    != 0) {
                    const int error = errno;
                    static_cast<void>(::close(listen_fd_));
                    listen_fd_ = -1;
                    throw std::runtime_error(
                        std::string("failed to inspect test socket: ")
                        + std::strerror(error));
                }
                endpoint_ = "http://127.0.0.1:"
                    + std::to_string(ntohs(address.sin_port));
                thread_ = std::thread([this] { serve(); });
            }

            LocalMcpServer(const LocalMcpServer&) = delete;
            LocalMcpServer& operator=(const LocalMcpServer&) = delete;

            ~LocalMcpServer() {
                if (listen_fd_ >= 0) {
                    static_cast<void>(::shutdown(listen_fd_, SHUT_RDWR));
                    static_cast<void>(::close(listen_fd_));
                    listen_fd_ = -1;
                }
                if (thread_.joinable()) {
                    thread_.join();
                }
            }

            [[nodiscard]] const std::string& endpoint() const noexcept {
                return endpoint_;
            }

            [[nodiscard]] std::vector<std::string> wait_for_requests() {
                if (thread_.joinable()) {
                    thread_.join();
                }
                if (failure_) {
                    std::rethrow_exception(failure_);
                }
                return requests_;
            }

        private:
            [[nodiscard]] static std::string read_request(int fd) {
                std::string request;
                char buffer[4096];
                std::size_t body_size = 0;
                for (;;) {
                    const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
                    if (received <= 0) {
                        throw std::runtime_error("incomplete test HTTP request");
                    }
                    request.append(buffer, static_cast<std::size_t>(received));
                    const std::size_t header_end = request.find("\r\n\r\n");
                    if (header_end == std::string::npos) {
                        continue;
                    }
                    const std::size_t length_start = request.find(
                        "Content-Length:");
                    if (length_start == std::string::npos) {
                        throw std::runtime_error("test request has no content length");
                    }
                    const std::size_t value_start = length_start
                        + std::string("Content-Length:").size();
                    const std::size_t value_end = request.find("\r\n", value_start);
                    body_size = static_cast<std::size_t>(std::stoull(
                        request.substr(value_start, value_end - value_start)));
                    if (request.size() >= header_end + 4 + body_size) {
                        return request;
                    }
                }
            }

            static void send_all(int fd, std::string_view response) {
                std::size_t offset = 0;
                while (offset < response.size()) {
                    const ssize_t sent = ::send(
                        fd,
                        response.data() + offset,
                        response.size() - offset,
                        MSG_NOSIGNAL);
                    if (sent <= 0) {
                        throw std::runtime_error("failed to send test response");
                    }
                    offset += static_cast<std::size_t>(sent);
                }
            }

            static std::string response_for(std::size_t request_index) {
                static constexpr std::string_view bodies[] = {
                    R"({"jsonrpc":"2.0","id":1,"result":{}})",
                    R"({"jsonrpc":"2.0","result":{}})",
                    R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"book_ticker","description":"best bid best ask","annotations":{"readOnlyHint":true},"inputSchema":{"type":"object","properties":{"symbol":{"type":"string"}}}}]}})",
                    R"({"jsonrpc":"2.0","id":3,"result":{"structuredContent":{"symbol":"BTCUSDT","bidPrice":"100.00","askPrice":"101.00"}}})",
                };
                const std::string_view body = bodies[request_index];
                return "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Mcp-Session-Id: local-test-session\r\n"
                    "Content-Length: " + std::to_string(body.size())
                    + "\r\nConnection: close\r\n\r\n" + std::string(body);
            }

            void serve() noexcept {
                try {
                    for (std::size_t index = 0; index < 4; ++index) {
                        const int client_fd = ::accept4(
                            listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                        if (client_fd < 0) {
                            throw std::runtime_error("failed to accept test request");
                        }
                        try {
                            requests_.push_back(read_request(client_fd));
                            send_all(client_fd, response_for(index));
                        } catch (...) {
                            static_cast<void>(::close(client_fd));
                            throw;
                        }
                        static_cast<void>(::close(client_fd));
                    }
                } catch (...) {
                    failure_ = std::current_exception();
                }
            }

            int listen_fd_{-1};
            std::string endpoint_;
            std::thread thread_;
            std::vector<std::string> requests_;
            std::exception_ptr failure_;
        };

        [[nodiscard]] bool has_authorization_header(
            const std::string& request,
            std::string_view expected_value) {
            return request.find(
                "Authorization: Bearer " + std::string(expected_value)
                + "\r\n") != std::string::npos;
        }

        class FakeBinanceAgentOsClient final : public BinanceAgentOsClient {
        public:
            explicit FakeBinanceAgentOsClient(ExternalMarketSnapshot snapshot)
                : snapshot_(std::move(snapshot)) {}

            ExternalMarketSnapshot fetch_market_snapshot(
                std::string_view symbol) override {
                ++calls_;
                last_symbol_ = symbol;
                return snapshot_;
            }

            std::size_t calls() const noexcept {
                return calls_;
            }

            const std::string& last_symbol() const noexcept {
                return last_symbol_;
            }

        private:
            ExternalMarketSnapshot snapshot_;
            std::size_t calls_{};
            std::string last_symbol_;
        };

        class FailingBinanceAgentOsClient final : public BinanceAgentOsClient {
        public:
            enum class Failure {
                Transport,
                Provider,
            };

            explicit FailingBinanceAgentOsClient(Failure failure)
                : failure_(failure) {}

            ExternalMarketSnapshot fetch_market_snapshot(
                std::string_view) override {
                if (failure_ == Failure::Transport) {
                    throw BinanceAgentOsTransportError("transport failed");
                }
                throw BinanceAgentOsProviderError("schema failed");
            }

        private:
            Failure failure_;
        };

        class ExternalMarketInspectingPolicy final : public AgentPolicy {
        public:
            AgentAction decide(
                const AgentObservation& observation) const override {
                if (observation.external_market.has_value()
                    && observation.external_market->best_ask == 12'345) {
                    return HoldAction{};
                }
                return SubmitOrderAction{Side::Buy, 1, 1};
            }
        };

        TEST(ExternalPriceConversionTest,
             ConvertsExactDecimalUsingConfiguredIntegerScale) {
            EXPECT_EQ(parse_scaled_decimal_price("123.45", 100), 12'345);
            EXPECT_EQ(parse_scaled_decimal_price("123.4500", 100), 12'345);
            EXPECT_EQ(parse_scaled_decimal_price("0.125", 8), 1);
            EXPECT_EQ(parse_scaled_decimal_price("7", 1'000), 7'000);
        }

        TEST(ExternalPriceConversionTest,
             RejectsPrecisionNotExactlyRepresentableByScale) {
            EXPECT_THROW(
                static_cast<void>(parse_scaled_decimal_price("1.001", 100)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(parse_scaled_decimal_price("0.0001", 100)),
                std::invalid_argument);
        }

        TEST(ExternalPriceConversionTest, RejectsInvalidOrNonPositiveDecimal) {
            for (const std::string_view value : {
                     "", ".1", "1.", "1.2.3", "-1", "+1", "1e2",
                     " 1", "0", "0.00"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_scaled_decimal_price(value, 100)),
                    std::invalid_argument)
                    << value;
            }
            EXPECT_THROW(
                static_cast<void>(parse_scaled_decimal_price("1", 0)),
                std::invalid_argument);
        }

        TEST(ExternalPriceConversionTest,
             DetectsOverflowBeforeReturningLocalPrice) {
            const std::string maximum = std::to_string(
                std::numeric_limits<Price>::max());
            EXPECT_EQ(parse_scaled_decimal_price(maximum, 1),
                      std::numeric_limits<Price>::max());
            EXPECT_THROW(
                static_cast<void>(parse_scaled_decimal_price(maximum, 10)),
                std::overflow_error);
            EXPECT_THROW(
                static_cast<void>(parse_scaled_decimal_price(
                    "999999999999999999999999999999999999999", 1)),
                std::overflow_error);
        }

        TEST(BinanceAgentOsClientTest,
             FakeClientReturnsConfiguredNormalizedSnapshot) {
            FakeBinanceAgentOsClient client(
                ExternalMarketSnapshot{"ETHUSDT", 345'600, 345'700});

            EXPECT_EQ(
                client.fetch_market_snapshot("ETHUSDT"),
                (ExternalMarketSnapshot{"ETHUSDT", 345'600, 345'700}));
            EXPECT_EQ(client.calls(), 1U);
            EXPECT_EQ(client.last_symbol(), "ETHUSDT");
        }

        TEST(BinanceAgentOsClientTest, ValidatesConfigurationWithoutCredentials) {
            EXPECT_THROW(
                BinanceMcpClient(BinanceMcpConfig{"", 100, std::chrono::seconds(1)}),
                std::invalid_argument);
            EXPECT_THROW(
                BinanceMcpClient(BinanceMcpConfig{
                    "https://agent.binance.com/mcp/agentic",
                    0,
                    std::chrono::seconds(1)}),
                std::invalid_argument);
            EXPECT_THROW(
                BinanceMcpClient(BinanceMcpConfig{
                    "https://agent.binance.com/mcp/agentic",
                    100,
                    std::chrono::milliseconds(0)}),
                std::invalid_argument);
        }

        TEST(BinanceMcpClientAuthTest,
             OmitsAuthorizationForNoTokenMarketDataRequest) {
            ScopedEnvironmentVariable no_token(
                "BINANCE_AGENT_OS_ACCESS_TOKEN", std::nullopt);
            LocalMcpServer server;
            BinanceMcpClient client(BinanceMcpConfig{
                server.endpoint(), 100, std::chrono::seconds(1)});

            EXPECT_EQ(
                client.fetch_market_snapshot("BTCUSDT"),
                (ExternalMarketSnapshot{"BTCUSDT", 10'000, 10'100}));

            const std::vector<std::string> requests =
                server.wait_for_requests();
            ASSERT_EQ(requests.size(), 4U);
            for (const std::string& request : requests) {
                EXPECT_EQ(request.find("Authorization:"), std::string::npos);
            }
        }

        TEST(BinanceMcpClientAuthTest,
             SendsBearerAuthorizationForConfiguredToken) {
            constexpr std::string_view token = "unit-test-token";
            ScopedEnvironmentVariable configured_token(
                "BINANCE_AGENT_OS_ACCESS_TOKEN", std::string(token));
            LocalMcpServer server;
            BinanceMcpClient client(BinanceMcpConfig{
                server.endpoint(), 100, std::chrono::seconds(1)});

            EXPECT_EQ(
                client.fetch_market_snapshot("BTCUSDT"),
                (ExternalMarketSnapshot{"BTCUSDT", 10'000, 10'100}));

            const std::vector<std::string> requests =
                server.wait_for_requests();
            ASSERT_EQ(requests.size(), 4U);
            for (const std::string& request : requests) {
                EXPECT_TRUE(has_authorization_header(request, token));
            }
        }

        TEST(BinanceAgentOsObservationBridgeTest,
             KeepsExternalAndLocalPricesDistinctAndReadOnly) {
            constexpr AgentId agent_id = 101;
            constexpr AccountId account_id = 1;
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};

            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(account_id));
            accounts.fund(account_id, 20, 7);
            accounts.fund(account_id, 10, 500);
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({agent_id, account_id}));
            OrderBook order_book;
            const auto trades = order_book.add_order(
                Order{1, Side::Sell, OrderType::Limit, 100, 2, 1});
            ASSERT_TRUE(trades.empty());
            Ledger ledger;
            AgentObservationService observations{
                registry, accounts, order_book, instrument};

            const AgentObservation local = observations.observe(agent_id);
            ASSERT_EQ(local.best_ask, 100);
            EXPECT_FALSE(local.external_market.has_value());
            const auto balance_before = accounts.find_balance(account_id, 20);
            const std::size_t orders_before = order_book.order_count();
            const std::size_t ledger_before = ledger.entries().size();

            FakeBinanceAgentOsClient client(
                ExternalMarketSnapshot{"BTCUSDT", 12'344, 12'345});
            BinanceAgentOsObservationBridge bridge(client, "BTCUSDT");
            const AgentObservation enriched =
                bridge.attach_external_market(local);

            EXPECT_EQ(enriched.best_ask, 100);
            ASSERT_TRUE(enriched.external_market.has_value());
            EXPECT_EQ(
                enriched.external_market,
                (ExternalMarketSnapshot{"BTCUSDT", 12'344, 12'345}));
            EXPECT_EQ(local.best_ask, 100);
            EXPECT_FALSE(local.external_market.has_value());
            EXPECT_EQ(order_book.order_count(), orders_before);
            EXPECT_EQ(accounts.find_balance(account_id, 20), balance_before);
            EXPECT_EQ(ledger.entries().size(), ledger_before);
        }

        TEST(BinanceAgentOsObservationBridgeTest,
             ExistingAgentPolicyCanInspectOptionalExternalMarket) {
            FakeBinanceAgentOsClient client(
                ExternalMarketSnapshot{"BTCUSDT", 12'344, 12'345});
            BinanceAgentOsObservationBridge bridge(client, "BTCUSDT");
            AgentObservation local{};
            local.best_ask = 100;

            const AgentObservation enriched =
                bridge.attach_external_market(local);
            const ExternalMarketInspectingPolicy policy;

            EXPECT_TRUE(std::holds_alternative<HoldAction>(
                policy.decide(enriched)));
            EXPECT_EQ(enriched.best_ask, 100);
        }

        TEST(BinanceAgentOsObservationBridgeTest,
             ExternalFailuresRemainDistinctAndDoNotCreateFallbackPrices) {
            AgentObservation local{};
            local.best_bid = 90;
            local.best_ask = 100;

            FailingBinanceAgentOsClient transport_client(
                FailingBinanceAgentOsClient::Failure::Transport);
            BinanceAgentOsObservationBridge transport_bridge(
                transport_client, "BTCUSDT");
            EXPECT_THROW(
                static_cast<void>(
                    transport_bridge.attach_external_market(local)),
                BinanceAgentOsTransportError);

            FailingBinanceAgentOsClient provider_client(
                FailingBinanceAgentOsClient::Failure::Provider);
            BinanceAgentOsObservationBridge provider_bridge(
                provider_client, "BTCUSDT");
            EXPECT_THROW(
                static_cast<void>(
                    provider_bridge.attach_external_market(local)),
                BinanceAgentOsProviderError);

            EXPECT_FALSE(local.external_market.has_value());
            EXPECT_EQ(local.best_bid, 90);
            EXPECT_EQ(local.best_ask, 100);
        }

        TEST(BinanceAgentOsObservationBridgeTest, RejectsEmptyConfiguredSymbol) {
            FakeBinanceAgentOsClient client(
                ExternalMarketSnapshot{"BTCUSDT", 1, 2});
            EXPECT_THROW(
                BinanceAgentOsObservationBridge(client, ""),
                std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
