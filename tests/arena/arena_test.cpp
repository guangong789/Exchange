#include "exchange/arena/arena.hpp"
#include "exchange/accounting/funding_coordinator.hpp"

#include <initializer_list>
#include <stdexcept>
#include <variant>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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

        class ThrowingPolicy final : public AgentPolicy {
        public:
            AgentAction decide(const AgentObservation&) const override {
                throw std::runtime_error("policy failure");
            }
        };

        struct ArenaWorld {
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
            ArenaWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent(
                AgentIdentity{agent_id, account_id}));
        }

        void set_up_two_agent_trading_world(ArenaWorld& world) {
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 10, 400);
            world.accounts.fund(account_b, 20, 4);
        }

        ArenaScenario scenario_for(
            std::uint64_t max_rounds,
            std::initializer_list<AgentId> agent_ids) {
            ArenaScenario scenario{ArenaConfig{max_rounds}, {}};
            for (const AgentId agent_id : agent_ids) {
                scenario.objectives.push_back(
                    AgentObjectiveAssignment{
                        agent_id,
                        AssetTargetObjective{20, 1}});
            }
            return scenario;
        }

        TEST(ArenaTest, RejectsInvalidConfigAndEmptyParticipants) {
            ArenaWorld world;
            HoldPolicy hold;
            create_agent(world, agent_a, account_a);

            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(0, {agent_a}),
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(1, {agent_a}),
                    {},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
        }

        TEST(ArenaTest, RejectsInvalidDuplicateAndUnknownParticipants) {
            ArenaWorld world;
            HoldPolicy hold;
            create_agent(world, agent_a, account_a);

            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(1, {agent_a}),
                    {{0, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(1, {agent_a}),
                    {{agent_a, nullptr}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(1, {agent_a}),
                    {{agent_a, &hold}, {agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    scenario_for(1, {999}),
                    {{999, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::out_of_range);
        }

        TEST(ArenaScenarioTest,
             RequiresOneValidObjectiveForEveryAndOnlyParticipant) {
            ArenaWorld world;
            HoldPolicy hold;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);

            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{{1}, {{0, {20, 1}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{{1}, {{agent_a, {0, 1}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{{1}, {{agent_a, {20, 0}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{
                        {1},
                        {{agent_a, {20, 1}}, {agent_a, {10, 1}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{{1}, {{999, {20, 1}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::out_of_range);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{
                        {1},
                        {{agent_a, {20, 1}}, {agent_b, {20, 1}}}},
                    {{agent_a, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(Arena(
                    ArenaScenario{{1}, {{agent_a, {20, 1}}}},
                    {{agent_a, &hold}, {agent_b, &hold}},
                    world.registry,
                    world.observations,
                    world.actions)),
                std::invalid_argument);
        }

        TEST(ArenaTest,
             PreservesConfiguredOrderAndRecordsEveryHoldAcrossRounds) {
            ArenaWorld world;
            HoldPolicy hold;
            for (const auto [agent_id, account_id] :
                 {std::pair{AgentId{5}, AccountId{50}},
                  std::pair{AgentId{2}, AccountId{20}},
                  std::pair{AgentId{9}, AccountId{90}}}) {
                create_agent(world, agent_id, account_id);
            }
            Arena arena(
                scenario_for(2, {5, 2, 9}),
                {{5, &hold}, {2, &hold}, {9, &hold}},
                world.registry,
                world.observations,
                world.actions);

            EXPECT_EQ(arena.current_round(), 0U);
            arena.run();

            EXPECT_EQ(arena.current_round(), 2U);
            ASSERT_EQ(arena.trace().size(), 6U);
            const AgentId expected_agents[]{5, 2, 9, 5, 2, 9};
            const std::uint64_t expected_rounds[]{1, 1, 1, 2, 2, 2};
            for (std::size_t index = 0; index < arena.trace().size();
                 ++index) {
                EXPECT_EQ(arena.trace()[index].agent_id,
                          expected_agents[index]);
                EXPECT_EQ(arena.trace()[index].round,
                          expected_rounds[index]);
                EXPECT_TRUE(std::holds_alternative<HoldAction>(
                    arena.trace()[index].action));
                EXPECT_TRUE(std::holds_alternative<HoldActionResult>(
                    arena.trace()[index].result));
            }
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            EXPECT_THROW(arena.run(), std::logic_error);
        }

        TEST(ArenaTest,
             CrossAccountCancelRejectionIsRecordedAndNextAgentActs) {
            ArenaWorld world;
            HoldPolicy hold;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_b, 10, 500);
            const AgentActionResult resting = world.actions.execute(
                agent_b,
                SubmitOrderAction{Side::Buy, 100, 2});
            const OrderId order_id =
                std::get<SubmitActionResult>(resting).order_id;
            FixedActionPolicy foreign_cancel(
                CancelOrderAction{order_id});
            Arena arena(
                scenario_for(1, {agent_a, agent_b}),
                {{agent_a, &foreign_cancel}, {agent_b, &hold}},
                world.registry,
                world.observations,
                world.actions);

            arena.run();

            ASSERT_EQ(arena.trace().size(), 2U);
            EXPECT_EQ(arena.trace()[0].agent_id, agent_a);
            ASSERT_TRUE(std::holds_alternative<CancelActionResult>(
                arena.trace()[0].result));
            EXPECT_EQ(
                std::get<CancelActionResult>(arena.trace()[0].result).result,
                CancelResult::NotOwner);
            EXPECT_EQ(arena.trace()[1].agent_id, agent_b);
            EXPECT_TRUE(std::holds_alternative<HoldActionResult>(
                arena.trace()[1].result));
            EXPECT_TRUE(world.reservations.find(order_id).has_value());
        }

        TEST(ArenaTest, FatalPolicyExceptionPreservesCompletedTraceOnly) {
            ArenaWorld world;
            HoldPolicy hold;
            ThrowingPolicy throwing;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            Arena arena(
                scenario_for(2, {agent_a, agent_b}),
                {{agent_a, &hold}, {agent_b, &throwing}},
                world.registry,
                world.observations,
                world.actions);

            EXPECT_THROW(arena.run(), std::runtime_error);

            ASSERT_EQ(arena.trace().size(), 1U);
            EXPECT_EQ(arena.trace()[0].round, 1U);
            EXPECT_EQ(arena.trace()[0].agent_id, agent_a);
            EXPECT_EQ(arena.current_round(), 1U);
            EXPECT_THROW(arena.run(), std::logic_error);
        }

        TEST(ArenaIntegrationTest,
             MultiRoundSequentialTurnsUseFreshObservationsAndRealTrades) {
            constexpr AccountId treasury = 99;
            ArenaWorld world;
            ASSERT_TRUE(world.accounts.create_account(treasury));
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(treasury, 10, 400);
            world.accounts.fund(treasury, 20, 4);
            FundingCoordinator funding(treasury, world.accounts, world.ledger);
            ASSERT_EQ(
                funding.fund(account_a, 10, 400),
                FundingResult::Funded);
            ASSERT_EQ(
                funding.fund(account_b, 20, 4),
                FundingResult::Funded);

            FixedActionPolicy seller(
                SubmitOrderAction{Side::Sell, 100, 2});
            AcquireAssetPolicy buyer(2);
            Arena arena(
                ArenaScenario{
                    ArenaConfig{2},
                    {
                        {agent_b, {10, 400}},
                        {agent_a, {20, 4}},
                    }},
                {{agent_b, &seller}, {agent_a, &buyer}},
                world.registry,
                world.observations,
                world.actions);

            arena.run();

            ASSERT_EQ(arena.trace().size(), 4U);
            for (std::size_t index = 0; index < arena.trace().size(); ++index) {
                EXPECT_EQ(arena.trace()[index].round, index / 2 + 1);
                EXPECT_EQ(
                    arena.trace()[index].agent_id,
                    index % 2 == 0 ? agent_b : agent_a);
            }
            EXPECT_FALSE(arena.trace()[0].observation.best_ask.has_value());
            EXPECT_EQ(arena.trace()[1].observation.best_ask, 100);
            EXPECT_FALSE(arena.trace()[2].observation.best_ask.has_value());
            EXPECT_EQ(arena.trace()[3].observation.best_ask, 100);
            EXPECT_EQ(
                arena.trace()[0].observation.objective,
                (ObjectiveProgress{agent_b, 10, 0, 400, false}));
            EXPECT_EQ(
                arena.trace()[1].observation.objective,
                (ObjectiveProgress{agent_a, 20, 0, 4, false}));
            EXPECT_EQ(
                arena.trace()[2].observation.objective,
                (ObjectiveProgress{agent_b, 10, 200, 400, false}));
            EXPECT_EQ(
                arena.trace()[3].observation.objective,
                (ObjectiveProgress{agent_a, 20, 2, 4, false}));
            EXPECT_TRUE(std::holds_alternative<SubmitOrderAction>(
                arena.trace()[1].action));
            EXPECT_TRUE(std::holds_alternative<SubmitOrderAction>(
                arena.trace()[3].action));
            ASSERT_EQ(arena.outcomes().size(), 2U);
            EXPECT_EQ(arena.outcomes()[0].agent_id, agent_b);
            EXPECT_EQ(arena.outcomes()[0].objective,
                      (ObjectiveProgress{agent_b, 10, 400, 400, true}));
            EXPECT_EQ(arena.outcomes()[1].agent_id, agent_a);
            EXPECT_EQ(arena.outcomes()[1].objective,
                      (ObjectiveProgress{agent_a, 20, 4, 4, true}));

            EXPECT_EQ(
                world.accounts.find_balance(account_a, 10),
                (Balance{0, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_a, 20),
                (Balance{4, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_b, 20),
                (Balance{0, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_b, 10),
                (Balance{400, 0}));
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(world.ledger.entries().size(), 8U);
            EXPECT_TRUE(std::holds_alternative<FundingLedgerMetadata>(
                world.ledger.entries()[0].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<FundingLedgerMetadata>(
                world.ledger.entries()[1].transaction.metadata));
            for (std::size_t offset : {std::size_t{2}, std::size_t{5}}) {
                EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                    world.ledger.entries()[offset].transaction.metadata));
                EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                    world.ledger.entries()[offset + 1].transaction.metadata));
                EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                    world.ledger.entries()[offset + 2].transaction.metadata));
            }
        }

        TEST(ArenaDeterminismTest,
             FreshWorldsProduceIdenticalTraceBalancesLedgerAndMarket) {
            ArenaWorld first;
            ArenaWorld second;
            set_up_two_agent_trading_world(first);
            set_up_two_agent_trading_world(second);
            FixedActionPolicy first_seller(
                SubmitOrderAction{Side::Sell, 100, 2});
            FixedActionPolicy second_seller(
                SubmitOrderAction{Side::Sell, 100, 2});
            AcquireAssetPolicy first_buyer(2);
            AcquireAssetPolicy second_buyer(2);
            Arena first_arena(
                ArenaScenario{
                    ArenaConfig{2},
                    {
                        {agent_b, {10, 400}},
                        {agent_a, {20, 4}},
                    }},
                {{agent_b, &first_seller}, {agent_a, &first_buyer}},
                first.registry,
                first.observations,
                first.actions);
            Arena second_arena(
                ArenaScenario{
                    ArenaConfig{2},
                    {
                        {agent_b, {10, 400}},
                        {agent_a, {20, 4}},
                    }},
                {{agent_b, &second_seller}, {agent_a, &second_buyer}},
                second.registry,
                second.observations,
                second.actions);

            first_arena.run();
            second_arena.run();

            EXPECT_EQ(first_arena.trace(), second_arena.trace());
            EXPECT_EQ(first_arena.outcomes(), second_arena.outcomes());
            for (const AccountId account_id : {account_a, account_b}) {
                for (const AssetId asset_id : {AssetId{10}, AssetId{20}}) {
                    EXPECT_EQ(
                        first.accounts.find_balance(account_id, asset_id),
                        second.accounts.find_balance(account_id, asset_id));
                }
            }
            EXPECT_EQ(first.ledger.entries(), second.ledger.entries());
            EXPECT_EQ(
                first.matching_engine.order_book().best_bid(),
                second.matching_engine.order_book().best_bid());
            EXPECT_EQ(
                first.matching_engine.order_book().best_ask(),
                second.matching_engine.order_book().best_ask());
            EXPECT_EQ(
                first.matching_engine.order_book().order_count(),
                second.matching_engine.order_book().order_count());
        }

        TEST(ArenaObjectiveTest,
             TerminalOutcomeUsesFinalWorldStateInsteadOfLatchingObservation) {
            ArenaWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 10, 100);
            world.accounts.fund(account_b, 20, 1);
            FixedActionPolicy seller(
                SubmitOrderAction{Side::Sell, 100, 1});
            FixedActionPolicy buyer(
                SubmitOrderAction{Side::Buy, 100, 1});
            Arena arena(
                ArenaScenario{
                    ArenaConfig{1},
                    {
                        {agent_b, {10, 100}},
                        {agent_a, {10, 100}},
                    }},
                {{agent_b, &seller}, {agent_a, &buyer}},
                world.registry,
                world.observations,
                world.actions);

            arena.run();

            ASSERT_EQ(arena.trace().size(), 2U);
            ASSERT_TRUE(arena.trace()[1].observation.objective.has_value());
            EXPECT_TRUE(arena.trace()[1].observation.objective->achieved);
            EXPECT_EQ(arena.trace()[1].observation.objective->current_amount,
                      100);
            ASSERT_EQ(arena.outcomes().size(), 2U);
            EXPECT_EQ(arena.outcomes()[1].agent_id, agent_a);
            EXPECT_EQ(arena.outcomes()[1].objective.current_amount, 0);
            EXPECT_FALSE(arena.outcomes()[1].objective.achieved);
            EXPECT_EQ(
                world.accounts.find_balance(account_a, 10),
                (Balance{0, 0}));
        }
    }  // namespace
}  // namespace exchange
