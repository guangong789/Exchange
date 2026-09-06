#include "exchange/model/model_agent_policy.hpp"

#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "exchange/agent/agent_action_gateway.hpp"
#include "exchange/agent/agent_observation_service.hpp"
#include "exchange/arena/arena.hpp"
#include "exchange/model/model_decision.hpp"

namespace exchange {
    namespace {
        constexpr InstrumentContext test_instrument{
            20,
            10,
            1,
            1,
            1,
        };
        constexpr AgentId agent_a = 101;
        constexpr AgentId agent_b = 202;
        constexpr AccountId account_a = 1;
        constexpr AccountId account_b = 2;

        class FakeModelAdapter final : public ModelAdapter {
        public:
            explicit FakeModelAdapter(std::deque<ModelResponse> responses)
                : responses_(std::move(responses)) {}

            ModelResponse invoke(const ModelRequest& request) override {
                requests.push_back(request);
                if (responses_.empty()) {
                    throw ModelAdapterError(
                        "Fake adapter has no configured response");
                }
                ModelResponse response = std::move(responses_.front());
                responses_.pop_front();
                return response;
            }

            std::vector<ModelRequest> requests;

        private:
            std::deque<ModelResponse> responses_;
        };

        class HoldPolicy final : public AgentPolicy {
        public:
            AgentAction decide(const AgentObservation&) const override {
                return HoldAction{};
            }
        };

        class FixedActionPolicy final : public AgentPolicy {
        public:
            explicit FixedActionPolicy(AgentAction action)
                : action_(std::move(action)) {}

            AgentAction decide(const AgentObservation&) const override {
                return action_;
            }

        private:
            AgentAction action_;
        };

        struct ModelWorld {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator execution_coordinator{
                test_instrument,
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
                test_instrument};
            AgentActionGateway actions{
                registry,
                execution_coordinator};
        };

        void create_agent(
            ModelWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent({agent_id, account_id}));
        }

        TEST(ModelAgentPolicyTest,
             BuildsSemanticPromptAndConvertsStructuredResponseToAction) {
            FakeModelAdapter adapter({ModelResponse{
                R"({"action":"submit_order","side":"buy","price":95,"quantity":2})"}});
            const ModelAgentPolicy policy(adapter);
            const AgentObservation observation{
                agent_a,
                account_a,
                90,
                95,
                Balance{1, 2},
                Balance{7, 3},
                ObjectiveProgress{agent_a, 20, 1, 4, false},
                ExternalMarketSnapshot{"BTCUSDT", 6'000'000, 6'000'100},
            };

            EXPECT_EQ(
                policy.decide(observation),
                (AgentAction{SubmitOrderAction{Side::Buy, 95, 2}}));
            ASSERT_EQ(adapter.requests.size(), 1U);
            const ModelRequest& request = adapter.requests.front();
            EXPECT_NE(request.system_prompt.find("exactly one JSON object"),
                      std::string::npos);
            EXPECT_NE(request.system_prompt.find("submit_order"),
                      std::string::npos);
            EXPECT_NE(request.system_prompt.find("cancel_order"),
                      std::string::npos);
            EXPECT_NE(request.system_prompt.find("hold"), std::string::npos);
            EXPECT_NE(request.user_prompt.find("Agent ID: 101"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("Account ID: 1"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("available=1, reserved=2"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("available=7, reserved=3"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("Best bid: 90"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("Best ask: 95"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find(
                          "External market: symbol=BTCUSDT, best bid=6000000, "
                          "best ask=6000100"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("target asset=20"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("current holding=1"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("target holding=4"),
                      std::string::npos);
            EXPECT_NE(request.user_prompt.find("achieved=false"),
                      std::string::npos);
        }

        TEST(ModelAgentPolicyIntegrationTest,
             FinanciallyInvalidStructuredActionsAreRejectedByCoreAndArenaContinues) {
            ModelWorld world;
            create_agent(world, agent_a, account_a);
            world.accounts.fund(account_a, 10, 5);
            FakeModelAdapter adapter({
                {R"({"action":"submit_order","side":"buy","price":100,"quantity":10})"},
                {R"({"action":"submit_order","side":"buy","price":100,"quantity":10})"},
            });
            ModelAgentPolicy model_policy(adapter);
            Arena arena(
                ArenaScenario{{2}, {{agent_a, {20, 1}}}},
                {{agent_a, &model_policy}},
                world.registry,
                world.observations,
                world.actions);

            arena.run();

            ASSERT_EQ(arena.trace().size(), 2U);
            for (const ArenaTurnRecord& turn : arena.trace()) {
                ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(
                    turn.result));
                EXPECT_EQ(
                    std::get<SubmitActionResult>(turn.result).result,
                    SubmitResult::InsufficientFunds);
            }
            EXPECT_EQ(adapter.requests.size(), 2U);
            EXPECT_EQ(world.accounts.find_balance(account_a, 10),
                      (Balance{5, 0}));
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(ModelAgentPolicyIntegrationTest,
             MalformedDecisionPropagatesAndPreservesEarlierTraceAndWorld) {
            ModelWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            HoldPolicy hold;
            FakeModelAdapter adapter({ModelResponse{"not json"}});
            ModelAgentPolicy model_policy(adapter);
            Arena arena(
                ArenaScenario{
                    {1},
                    {{agent_a, {20, 1}}, {agent_b, {20, 1}}}},
                {{agent_a, &hold}, {agent_b, &model_policy}},
                world.registry,
                world.observations,
                world.actions);

            EXPECT_THROW(arena.run(), ModelDecisionParseError);

            ASSERT_EQ(arena.trace().size(), 1U);
            EXPECT_EQ(arena.trace()[0].agent_id, agent_a);
            EXPECT_TRUE(std::holds_alternative<HoldActionResult>(
                arena.trace()[0].result));
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(ModelAgentPolicyIntegrationTest,
             ModelAndScriptedPoliciesTradeThroughTheSameDeterministicCore) {
            ModelWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 10, 400);
            world.accounts.fund(account_b, 20, 4);
            FakeModelAdapter adapter({
                {R"({"action":"submit_order","side":"buy","price":100,"quantity":2})"},
                {R"({"action":"submit_order","side":"buy","price":100,"quantity":2})"},
            });
            ModelAgentPolicy model_policy(adapter);
            FixedActionPolicy seller(
                SubmitOrderAction{Side::Sell, 100, 2});
            Arena arena(
                ArenaScenario{
                    {2},
                    {{agent_b, {10, 400}}, {agent_a, {20, 4}}}},
                {{agent_b, &seller}, {agent_a, &model_policy}},
                world.registry,
                world.observations,
                world.actions);

            arena.run();

            ASSERT_EQ(arena.trace().size(), 4U);
            EXPECT_EQ(adapter.requests.size(), 2U);
            for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
                EXPECT_TRUE(std::holds_alternative<SubmitOrderAction>(
                    arena.trace()[index].action));
                EXPECT_EQ(
                    std::get<SubmitOrderAction>(arena.trace()[index].action),
                    (SubmitOrderAction{Side::Buy, 100, 2}));
                EXPECT_EQ(
                    std::get<SubmitActionResult>(arena.trace()[index].result)
                        .result,
                    SubmitResult::Accepted);
            }
            EXPECT_EQ(world.accounts.find_balance(account_a, 10),
                      (Balance{0, 0}));
            EXPECT_EQ(world.accounts.find_balance(account_a, 20),
                      (Balance{4, 0}));
            EXPECT_EQ(world.accounts.find_balance(account_b, 20),
                      (Balance{0, 0}));
            EXPECT_EQ(world.accounts.find_balance(account_b, 10),
                      (Balance{400, 0}));
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(world.ledger.entries().size(), 6U);
            for (std::size_t offset : {std::size_t{0}, std::size_t{3}}) {
                EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                    world.ledger.entries()[offset].transaction.metadata));
                EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                    world.ledger.entries()[offset + 1].transaction.metadata));
                EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                    world.ledger.entries()[offset + 2].transaction.metadata));
            }
            ASSERT_EQ(arena.outcomes().size(), 2U);
            EXPECT_TRUE(arena.outcomes()[0].objective.achieved);
            EXPECT_TRUE(arena.outcomes()[1].objective.achieved);
        }
    }  // namespace
}  // namespace exchange
