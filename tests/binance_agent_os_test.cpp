#include "exchange/binance_agent_os_client.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "exchange/account_store.hpp"
#include "exchange/agent_observation_service.hpp"
#include "exchange/agent_registry.hpp"
#include "exchange/ledger.hpp"
#include "exchange/order_book.hpp"

namespace exchange {
    namespace {
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
