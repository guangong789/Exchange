#include "exchange/agent_objective.hpp"
#include "exchange/agent_observation_service.hpp"
#include "exchange/event_collector.hpp"
#include "exchange/matching_engine.hpp"

#include <limits>
#include <stdexcept>
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

        TEST(ObjectiveEvaluatorTest,
             RejectsInvalidObjectivesUnknownAgentsAndMissingAccounts) {
            AccountStore accounts;
            AgentRegistry registry;
            ASSERT_TRUE(accounts.create_account(account_a));
            ASSERT_TRUE(registry.register_agent({agent_a, account_a}));
            ASSERT_TRUE(registry.register_agent({agent_b, account_b}));
            const ObjectiveEvaluator evaluator(registry, accounts);

            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(0, AssetTargetObjective{20, 1})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(agent_a, AssetTargetObjective{0, 1})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(agent_a, AssetTargetObjective{20, 0})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(agent_a, AssetTargetObjective{20, -1})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(999, AssetTargetObjective{20, 1})),
                std::out_of_range);
            EXPECT_THROW(
                static_cast<void>(
                    evaluator.evaluate(agent_b, AssetTargetObjective{20, 1})),
                std::logic_error);
        }

        TEST(ObjectiveEvaluatorTest,
             MissingAssetIsZeroAndEvaluationDoesNotCreateBalanceState) {
            AccountStore accounts;
            AgentRegistry registry;
            ASSERT_TRUE(accounts.create_account(account_a));
            ASSERT_TRUE(registry.register_agent({agent_a, account_a}));
            const ObjectiveEvaluator evaluator(registry, accounts);

            EXPECT_FALSE(accounts.find_balance(account_a, 20).has_value());
            EXPECT_EQ(
                evaluator.evaluate(agent_a, AssetTargetObjective{20, 1}),
                (ObjectiveProgress{agent_a, 20, 0, 1, false}));
            EXPECT_FALSE(accounts.find_balance(account_a, 20).has_value());
        }

        TEST(ObjectiveEvaluatorTest,
             CountsAvailableAndReservedWithoutMutatingBalance) {
            AccountStore accounts;
            AgentRegistry registry;
            ASSERT_TRUE(accounts.create_account(account_a));
            ASSERT_TRUE(registry.register_agent({agent_a, account_a}));
            accounts.fund(account_a, 20, 10);
            ASSERT_EQ(accounts.reserve(account_a, 20, 3), ReserveResult::Success);
            const Balance before = *accounts.find_balance(account_a, 20);
            const ObjectiveEvaluator evaluator(registry, accounts);

            EXPECT_EQ(
                evaluator.evaluate(agent_a, AssetTargetObjective{20, 10}),
                (ObjectiveProgress{agent_a, 20, 10, 10, true}));
            EXPECT_EQ(accounts.find_balance(account_a, 20), before);
        }

        TEST(ObjectiveEvaluatorTest,
             CheckedHoldingOverflowFailsWithoutMutation) {
            AccountStore accounts;
            AgentRegistry registry;
            ASSERT_TRUE(accounts.create_account(account_a));
            ASSERT_TRUE(registry.register_agent({agent_a, account_a}));
            accounts.fund(account_a, 20, 2);
            ASSERT_EQ(accounts.reserve(account_a, 20, 1), ReserveResult::Success);
            accounts.credit_available(
                account_a,
                20,
                std::numeric_limits<Amount>::max() - 1);
            const Balance before = *accounts.find_balance(account_a, 20);
            const ObjectiveEvaluator evaluator(registry, accounts);

            EXPECT_THROW(
                static_cast<void>(evaluator.evaluate(
                    agent_a,
                    AssetTargetObjective{20, 1})),
                std::overflow_error);
            EXPECT_EQ(accounts.find_balance(account_a, 20), before);
        }

        TEST(ObjectiveObservationTest,
             ExposesOnlyRequestedAgentsOwnFreshObjectiveProgress) {
            AccountStore accounts;
            AgentRegistry registry;
            EventCollector events;
            MatchingEngine matching_engine(events);
            ASSERT_TRUE(accounts.create_account(account_a));
            ASSERT_TRUE(accounts.create_account(account_b));
            ASSERT_TRUE(registry.register_agent({agent_a, account_a}));
            ASSERT_TRUE(registry.register_agent({agent_b, account_b}));
            accounts.fund(account_a, 20, 2);
            accounts.fund(account_b, 10, 900);
            const AgentObservationService observations(
                registry,
                accounts,
                matching_engine.order_book(),
                test_instrument);

            EXPECT_FALSE(observations.observe(agent_a).objective.has_value());
            const AgentObservation observed_a = observations.observe(
                agent_a,
                AssetTargetObjective{20, 3});
            ASSERT_TRUE(observed_a.objective.has_value());
            EXPECT_EQ(
                *observed_a.objective,
                (ObjectiveProgress{agent_a, 20, 2, 3, false}));
            EXPECT_NE(observed_a.objective->agent_id, agent_b);
            EXPECT_NE(observed_a.objective->asset_id, AssetId{10});

            accounts.credit_available(account_a, 20, 1);
            EXPECT_EQ(
                observations.observe(agent_a, AssetTargetObjective{20, 3})
                    .objective,
                (ObjectiveProgress{agent_a, 20, 3, 3, true}));
        }

        TEST(ObjectivePolicyTest,
             HoldsWhenAchievedOrNoAskAndBuysAtAskWhenProgressing) {
            EXPECT_THROW(AcquireAssetPolicy(0), std::invalid_argument);
            EXPECT_THROW(AcquireAssetPolicy(-1), std::invalid_argument);
            const AcquireAssetPolicy policy(2);
            AgentObservation observation{};

            EXPECT_THROW(
                static_cast<void>(policy.decide(observation)),
                std::logic_error);
            observation.objective = ObjectiveProgress{agent_a, 20, 0, 2, false};
            EXPECT_EQ(policy.decide(observation), AgentAction{HoldAction{}});
            observation.best_ask = 100;
            EXPECT_EQ(
                policy.decide(observation),
                (AgentAction{SubmitOrderAction{Side::Buy, 100, 2}}));
            observation.objective->current_amount = 2;
            observation.objective->achieved = true;
            EXPECT_EQ(policy.decide(observation), AgentAction{HoldAction{}});
        }
    }  // namespace
}  // namespace exchange
