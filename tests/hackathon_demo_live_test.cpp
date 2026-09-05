#include "exchange/model/deepseek_adapter.hpp"
#include "exchange/hackathon/hackathon_demo.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext live_instrument{20, 10, 1, 1, 1};
        constexpr AgentId live_trader_agent = 101;
        constexpr AgentId live_analyst_agent = 202;
        constexpr AgentId live_risk_agent = 303;

        struct LiveDemoWorld {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator execution{
                live_instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            AgentRegistry registry;
            AgentObservationService observations{
                registry,
                accounts,
                matching_engine.order_book(),
                live_instrument};
            AgentActionGateway actions{registry, execution};
        };

        class CountingLiveModelAdapter final : public ModelAdapter {
        public:
            explicit CountingLiveModelAdapter(ModelAdapter& adapter) noexcept
                : adapter_(adapter) {}

            ModelResponse invoke(const ModelRequest& request) override {
                ++calls;
                last_request = request;
                return adapter_.invoke(request);
            }

            ModelAdapter& adapter_;
            std::size_t calls{};
            std::optional<ModelRequest> last_request;
        };

        class HackathonLiveIntegrationTest : public ::testing::Test {
        protected:
            void SetUp() override {
                const char* deepseek_key = std::getenv("DEEPSEEK_API_KEY");
                if (deepseek_key == nullptr || deepseek_key[0] == '\0') {
                    GTEST_SKIP()
                        << "DEEPSEEK_API_KEY is required";
                }
            }
        };

        void initialize_live_world(LiveDemoWorld& world) {
            ASSERT_TRUE(world.accounts.create_account(1));
            ASSERT_TRUE(world.accounts.create_account(2));
            ASSERT_TRUE(world.accounts.create_account(3));
            ASSERT_TRUE(world.registry.register_agent({live_trader_agent, 1}));
            ASSERT_TRUE(world.registry.register_agent({live_analyst_agent, 2}));
            ASSERT_TRUE(world.registry.register_agent({live_risk_agent, 3}));
            world.accounts.fund(1, 10, 500);
            world.accounts.fund(2, 20, 2);
            ASSERT_EQ(
                world.actions.execute(
                    live_analyst_agent,
                    SubmitOrderAction{Side::Sell, 100, 1}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));
        }

        TEST_F(HackathonLiveIntegrationTest,
               UsesDeterministicMarketAndOneLiveDeepSeekCognitionCall) {
            LiveDemoWorld world;
            initialize_live_world(world);
            const ExternalMarketSnapshot snapshot{
                "BTCUSDT", 6'000'000, 6'000'100};

            DeepSeekAdapter deepseek;
            CountingLiveModelAdapter counted(deepseek);
            ModelAgentPolicy model_policy(counted);
            ModelPremiumSignalPolicy trader_policy(model_policy);
            DeterministicRiskAgent risk_agent(2'000);
            HackathonDemoScenario scenario(
                HackathonDemoConfig{
                    live_trader_agent,
                    live_analyst_agent,
                    live_risk_agent,
                    AssetTargetObjective{20, 1},
                },
                PremiumInformationService(
                    ExternalPaymentRequirement{
                        2,
                        "http://127.0.0.1/premium-signal",
                        "Analyst Premium Market Signal",
                        "application/json",
                        "exact",
                        "eip155:97",
                        "10000",
                        "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                        "0x1111111111111111111111111111111111111111",
                        60,
                    },
                    PremiumMarketSignal{
                        "BUY_BASE",
                        95,
                        "Local ask is within the deterministic entry threshold",
                    }),
                world.registry,
                world.observations,
                world.actions,
                risk_agent,
                trader_policy);

            const HackathonDemoTurn turn = scenario.run_once(
                [](const ExternalPaymentRequirement& requirement) {
                    return ExternalPaymentPreview{
                        ExternalPaymentPreviewStatus::Approved,
                        "deterministic-demo-preview",
                        requirement.network,
                        requirement.asset,
                        requirement.amount,
                        "READY_TO_SIGN",
                        {},
                    };
                },
                snapshot);

            ASSERT_EQ(counted.calls, 1U);
            ASSERT_TRUE(counted.last_request.has_value());
            EXPECT_NE(
                counted.last_request->user_prompt.find(
                    "External market: symbol=" + snapshot.symbol
                    + ", best bid=" + std::to_string(snapshot.best_bid)
                    + ", best ask=" + std::to_string(snapshot.best_ask)),
                std::string::npos);
            ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
            EXPECT_EQ(
                std::get<SubmitOrderAction>(turn.action),
                (SubmitOrderAction{Side::Buy, 100, 1}));
            EXPECT_EQ(
                turn.action_result,
                (AgentActionResult{SubmitActionResult{
                    2,
                    2,
                    SubmitResult::Accepted}}));
            EXPECT_TRUE(turn.trader_objective.achieved);
            ASSERT_EQ(world.ledger.entries().size(), 3U);
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                world.ledger.entries().back().transaction.metadata));
        }
    }  // namespace
}  // namespace exchange
