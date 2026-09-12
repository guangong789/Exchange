#include "agent/provider/deepseek/deepseek_decision_provider.hpp"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        AgentObservation sample_observation() {
            AgentObservation observation;
            observation.agent_id = 101;
            observation.account_id = 7;
            observation.world = WorldState{
                3,
                InternalMarketState{90, 95},
                ExternalMarketState{
                    ExternalMarketSource::BinanceAlpha,
                    "ALPHA_426USDT",
                    ExternalPrice{4'070'000, 8},
                    ExternalPrice{4'060'000, 8},
                    ExternalPrice{4'080'000, 8},
                    1'773'110'025'000,
                    1'773'110'025'010,
                    ExternalMarketFreshness::Fresh},
            };
            observation.base_balance = Balance{1, 2};
            observation.quote_balance = Balance{700, 50};
            observation.active_orders.push_back(
                ObservedOrder{12, Side::Sell, 110, 2});
            observation.objective =
                ObjectiveProgress{101, 20, 3, 5, false};
            return observation;
        }

        TEST(DeepSeekDecisionProviderTest,
             UsesProviderInterfaceAndParsesStructuredAction) {
            DeepSeekPrompt captured;
            DeepSeekDecisionProvider deepseek(
                [&](const DeepSeekPrompt& prompt) {
                    captured = prompt;
                    return std::string(
                        R"({"action":"submit_order","side":"buy","price":95,"quantity":2})");
                });
            const AgentDecisionProvider& provider = deepseek;

            EXPECT_EQ(
                provider.decide(sample_observation()),
                (AgentAction{SubmitOrderAction{Side::Buy, 95, 2}}));
            EXPECT_NE(captured.system.find("one JSON object"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("cannot modify balances"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("Step: 3"), std::string::npos);
            EXPECT_NE(captured.user.find("Agent ID: 101"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("id=12"), std::string::npos);
            EXPECT_NE(captured.user.find("target=5"), std::string::npos);
            EXPECT_NE(captured.system.find("not trading on Binance"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("internal exchange"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("symbol=ALPHA_426USDT"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("latest_trade=0.04070000"),
                      std::string::npos);
        }

        TEST(DeepSeekActionParsingTest, ParsesHoldAndCancel) {
            EXPECT_EQ(
                parse_deepseek_action(R"({"action":"hold"})"),
                AgentAction{HoldAction{}});
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"cancel_order","order_id":12})"),
                AgentAction{CancelOrderAction{12}});
        }

        TEST(DeepSeekActionParsingTest, RejectsMalformedJsonAndUnknownAction) {
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action("not json")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"wait"})")),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekActionParsingTest, RejectsMissingAndUnexpectedFields) {
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"submit_order","side":"buy","price":95})")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"hold","quantity":1})")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"submit_order","side":"buy","price":95,"quantity":1,"account_id":9})")),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekActionParsingTest, RejectsInvalidQuantityAndPrice) {
            for (const std::string& response : {
                     R"({"action":"submit_order","side":"buy","price":0,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":0})",
                     R"({"action":"submit_order","side":"buy","price":-1,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":-1})",
                     R"({"action":"submit_order","side":"BUY","price":1,"quantity":1})"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_deepseek_action(response)),
                    DeepSeekDecisionError);
            }

            const std::string out_of_range =
                R"({"action":"submit_order","side":"buy","price":)"
                + std::to_string(
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    + 1)
                + R"(,"quantity":1})";
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(out_of_range)),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekDecisionProviderTest, RejectsMissingCompletionFunction) {
            EXPECT_THROW(
                static_cast<void>(DeepSeekDecisionProvider({})),
                std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
