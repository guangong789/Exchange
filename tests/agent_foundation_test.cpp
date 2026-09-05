#include "exchange/agent/agent.hpp"
#include "exchange/agent/agent_action_gateway.hpp"
#include "exchange/agent/agent_observation_service.hpp"
#include "exchange/agent/agent_registry.hpp"
#include "exchange/accounting/funding_coordinator.hpp"

#include <stdexcept>
#include <type_traits>
#include <variant>

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

        struct AgentWorld {
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
            AgentWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent(
                AgentIdentity{agent_id, account_id}));
        }

        TEST(AgentRegistryTest,
             EnforcesOneToOneValidIdentityAndReturnsSnapshots) {
            AgentRegistry registry;

            EXPECT_TRUE(registry.register_agent({agent_a, account_a}));
            EXPECT_FALSE(registry.register_agent({agent_a, account_b}));
            EXPECT_FALSE(registry.register_agent({agent_b, account_a}));
            EXPECT_EQ(
                registry.find(agent_a),
                (AgentIdentity{agent_a, account_a}));
            EXPECT_FALSE(registry.find(agent_b).has_value());

            auto snapshot = registry.find(agent_a);
            ASSERT_TRUE(snapshot.has_value());
            snapshot->account_id = 999;
            EXPECT_EQ(
                registry.find(agent_a),
                (AgentIdentity{agent_a, account_a}));
        }

        TEST(AgentRegistryTest, RejectsZeroIdsWithoutCreatingBindings) {
            AgentRegistry registry;

            EXPECT_THROW(
                static_cast<void>(registry.register_agent({0, account_a})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(registry.register_agent({agent_a, 0})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(registry.find(0)),
                std::invalid_argument);
            EXPECT_FALSE(registry.find(agent_a).has_value());
        }

        TEST(AgentObservationTest,
             ExposesOwnBalancesAndReadOnlyBestPricesOnly) {
            AgentWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 20, 7);
            world.accounts.fund(account_a, 10, 500);
            world.accounts.fund(account_b, 20, 99);
            world.accounts.fund(account_b, 10, 999);

            const auto sell_result = world.actions.execute(
                agent_a,
                SubmitOrderAction{Side::Sell, 100, 2});
            EXPECT_EQ(
                sell_result,
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));
            const auto buy_result = world.actions.execute(
                agent_b,
                SubmitOrderAction{Side::Buy, 90, 1});
            EXPECT_EQ(
                buy_result,
                (AgentActionResult{SubmitActionResult{
                    2,
                    2,
                    SubmitResult::Accepted}}));

            const AgentObservation observed_a =
                world.observations.observe(agent_a);
            EXPECT_EQ(observed_a.agent_id, agent_a);
            EXPECT_EQ(observed_a.account_id, account_a);
            EXPECT_EQ(observed_a.best_bid, 90);
            EXPECT_EQ(observed_a.best_ask, 100);
            EXPECT_EQ(observed_a.base_balance, (Balance{5, 2}));
            EXPECT_EQ(observed_a.quote_balance, (Balance{500, 0}));

            const AgentObservation observed_b =
                world.observations.observe(agent_b);
            EXPECT_EQ(observed_b.account_id, account_b);
            EXPECT_EQ(observed_b.base_balance, (Balance{99, 0}));
            EXPECT_EQ(observed_b.quote_balance, (Balance{909, 90}));
            EXPECT_NE(observed_b.base_balance, observed_a.base_balance);
        }

        TEST(AgentFoundationTest, UnknownAgentObservationAndActionFail) {
            AgentWorld world;

            EXPECT_THROW(
                static_cast<void>(world.observations.observe(999)),
                std::out_of_range);
            EXPECT_THROW(
                static_cast<void>(
                    world.actions.execute(999, HoldAction{})),
                std::out_of_range);
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(AgentActionGatewayTest,
             HoldDoesNotMutateWorldOrConsumeSubmitSequence) {
            AgentWorld world;
            create_agent(world, agent_a, account_a);
            world.accounts.fund(account_a, 10, 500);

            EXPECT_EQ(
                world.actions.execute(agent_a, HoldAction{}),
                AgentActionResult{HoldActionResult{}});
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);

            EXPECT_EQ(
                world.actions.execute(
                    agent_a,
                    SubmitOrderAction{Side::Buy, 100, 1}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));
        }

        TEST(AgentActionGatewayTest,
             SubmitAndCancelUseResolvedAccountAndExistingOwnershipRules) {
            AgentWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 10, 500);

            const AgentActionResult submitted = world.actions.execute(
                agent_a,
                SubmitOrderAction{Side::Buy, 100, 2});
            ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(submitted));
            const OrderId order_id =
                std::get<SubmitActionResult>(submitted).order_id;
            ASSERT_TRUE(world.reservations.find(order_id).has_value());
            EXPECT_EQ(world.reservations.find(order_id)->account_id, account_a);
            EXPECT_EQ(
                world.actions.execute(
                    agent_b,
                    CancelOrderAction{order_id}),
                AgentActionResult{CancelActionResult{CancelResult::NotOwner}});
            EXPECT_TRUE(world.reservations.find(order_id).has_value());

            EXPECT_EQ(
                world.actions.execute(
                    agent_a,
                    CancelOrderAction{order_id}),
                AgentActionResult{CancelActionResult{CancelResult::Cancelled}});
            EXPECT_FALSE(world.reservations.find(order_id).has_value());
            EXPECT_EQ(
                world.accounts.find_balance(account_a, 10),
                (Balance{500, 0}));
            ASSERT_EQ(world.ledger.entries().size(), 2U);
            EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                world.ledger.entries()[0].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<ReleaseLedgerMetadata>(
                world.ledger.entries()[1].transaction.metadata));
        }

        TEST(AgentPolicyTest, DeterministicallyBuysAtEligibleAskOtherwiseHolds) {
            EXPECT_THROW(ThresholdBuyPolicy(0, 1), std::invalid_argument);
            EXPECT_THROW(ThresholdBuyPolicy(100, 0), std::invalid_argument);
            const ThresholdBuyPolicy policy(100, 3);

            AgentObservation no_ask{};
            EXPECT_EQ(policy.decide(no_ask), AgentAction{HoldAction{}});
            no_ask.best_ask = 101;
            EXPECT_EQ(policy.decide(no_ask), AgentAction{HoldAction{}});
            no_ask.best_ask = 100;
            EXPECT_EQ(
                policy.decide(no_ask),
                (AgentAction{SubmitOrderAction{Side::Buy, 100, 3}}));
        }

        TEST(AgentFoundationIntegrationTest,
             ObserveDecideActLetsTwoAgentsTradeThroughExistingCore) {
            constexpr AccountId treasury = 99;
            AgentWorld world;
            ASSERT_TRUE(world.accounts.create_account(treasury));
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(treasury, 10, 1'000);
            world.accounts.fund(treasury, 20, 10);
            FundingCoordinator funding(treasury, world.accounts, world.ledger);
            ASSERT_EQ(
                funding.fund(account_a, 10, 1'000),
                FundingResult::Funded);
            ASSERT_EQ(
                funding.fund(account_b, 20, 5),
                FundingResult::Funded);

            EXPECT_EQ(
                world.actions.execute(
                    agent_b,
                    SubmitOrderAction{Side::Sell, 100, 2}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));
            const AgentObservation observation =
                world.observations.observe(agent_a);
            ASSERT_EQ(observation.best_ask, 100);

            const ThresholdBuyPolicy policy(100, 2);
            const AgentAction decision = policy.decide(observation);
            EXPECT_EQ(
                decision,
                (AgentAction{SubmitOrderAction{Side::Buy, 100, 2}}));
            EXPECT_EQ(
                world.actions.execute(agent_a, decision),
                (AgentActionResult{SubmitActionResult{
                    2,
                    2,
                    SubmitResult::Accepted}}));

            EXPECT_EQ(
                world.accounts.find_balance(account_a, 10),
                (Balance{800, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_a, 20),
                (Balance{2, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_b, 20),
                (Balance{3, 0}));
            EXPECT_EQ(
                world.accounts.find_balance(account_b, 10),
                (Balance{200, 0}));
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            EXPECT_FALSE(world.reservations.find(1).has_value());
            EXPECT_FALSE(world.reservations.find(2).has_value());

            ASSERT_EQ(world.ledger.entries().size(), 5U);
            EXPECT_TRUE(std::holds_alternative<FundingLedgerMetadata>(
                world.ledger.entries()[0].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<FundingLedgerMetadata>(
                world.ledger.entries()[1].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                world.ledger.entries()[2].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                world.ledger.entries()[3].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                world.ledger.entries()[4].transaction.metadata));
        }

        TEST(AgentFoundationDeterminismTest,
             IdenticalWorldsAndActionOrderProduceIdenticalResults) {
            AgentWorld first;
            AgentWorld second;
            for (AgentWorld* world : {&first, &second}) {
                create_agent(*world, agent_a, account_a);
                create_agent(*world, agent_b, account_b);
                world->accounts.fund(account_a, 10, 1'000);
                world->accounts.fund(account_b, 20, 5);
            }

            const AgentAction sell =
                SubmitOrderAction{Side::Sell, 100, 2};
            const AgentAction buy =
                SubmitOrderAction{Side::Buy, 100, 2};
            EXPECT_EQ(
                first.actions.execute(agent_b, sell),
                second.actions.execute(agent_b, sell));
            EXPECT_EQ(
                first.observations.observe(agent_a),
                second.observations.observe(agent_a));
            EXPECT_EQ(
                first.actions.execute(agent_a, buy),
                second.actions.execute(agent_a, buy));

            for (const AccountId account_id : {account_a, account_b}) {
                for (const AssetId asset_id : {AssetId{10}, AssetId{20}}) {
                    EXPECT_EQ(
                        first.accounts.find_balance(account_id, asset_id),
                        second.accounts.find_balance(account_id, asset_id));
                }
            }
            EXPECT_EQ(first.ledger.entries(), second.ledger.entries());
            EXPECT_EQ(
                first.matching_engine.order_book().order_count(),
                second.matching_engine.order_book().order_count());
        }

        static_assert(!std::is_copy_constructible_v<AgentRegistry>);
        static_assert(!std::is_copy_constructible_v<AgentActionGateway>);
    }  // namespace
}  // namespace exchange
