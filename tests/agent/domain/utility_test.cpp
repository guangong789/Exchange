#include "agent/domain/utility.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        AgentObservation observation_with_balances(
            Balance base,
            Balance quote) {
            AgentObservation observation;
            observation.base_balance = base;
            observation.quote_balance = quote;
            return observation;
        }

        TEST(AgentUtilityTest,
             EvaluatesExactBelowAndAboveTargetInventory) {
            const AgentPreferenceProfile profile{10, 5, 2};

            EXPECT_EQ(
                evaluate_agent_utility(
                    observation_with_balances({7, 3}, {80, 20}),
                    profile),
                (AgentUtilityBreakdown{100, 50, 0, 150}));
            EXPECT_EQ(
                evaluate_agent_utility(
                    observation_with_balances({6, 2}, {100, 0}),
                    profile),
                (AgentUtilityBreakdown{100, 40, 4, 136}));
            EXPECT_EQ(
                evaluate_agent_utility(
                    observation_with_balances({10, 2}, {90, 10}),
                    profile),
                (AgentUtilityBreakdown{100, 60, 4, 156}));
        }

        TEST(AgentUtilityTest,
             MissingBalancesAreZeroAndEvaluationIsDeterministic) {
            const AgentPreferenceProfile profile{3, 7, 2};
            const AgentObservation observation;
            const AgentUtilityBreakdown expected{0, 0, 6, -6};

            EXPECT_EQ(evaluate_agent_utility(observation, profile), expected);
            EXPECT_EQ(evaluate_agent_utility(observation, profile), expected);
        }

        TEST(AgentUtilityTest, CalculatesPositiveNegativeAndZeroDelta) {
            EXPECT_EQ(calculate_agent_utility_delta(100, 125), 25);
            EXPECT_EQ(calculate_agent_utility_delta(100, 75), -25);
            EXPECT_EQ(calculate_agent_utility_delta(100, 100), 0);
        }

        TEST(AgentUtilityTest, RejectsInvalidPreferenceConfiguration) {
            EXPECT_THROW(
                validate_agent_preference_profile({-1, 1, 1}),
                std::invalid_argument);
            EXPECT_THROW(
                validate_agent_preference_profile({1, -1, 1}),
                std::invalid_argument);
            EXPECT_THROW(
                validate_agent_preference_profile({1, 1, -1}),
                std::invalid_argument);
        }

        TEST(AgentUtilityTest, DetectsBalanceAndValuationOverflow) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            EXPECT_THROW(
                static_cast<void>(evaluate_agent_utility(
                    observation_with_balances({maximum, 1}, {0, 0}),
                    {0, 1, 0})),
                std::overflow_error);
            EXPECT_THROW(
                static_cast<void>(evaluate_agent_utility(
                    observation_with_balances({maximum, 0}, {0, 0}),
                    {0, 2, 0})),
                std::overflow_error);
            EXPECT_THROW(
                static_cast<void>(evaluate_agent_utility(
                    observation_with_balances({0, 0}, {0, 0}),
                    {maximum, 0, 2})),
                std::overflow_error);
            EXPECT_THROW(
                static_cast<void>(evaluate_agent_utility(
                    observation_with_balances({1, 0}, {maximum, 0}),
                    {1, 1, 0})),
                std::overflow_error);
        }

        TEST(AgentUtilityTest, SupportsLargeExactValuesAndDetectsDeltaOverflow) {
            constexpr UtilityValue maximum =
                std::numeric_limits<UtilityValue>::max();
            const AgentUtilityBreakdown value = evaluate_agent_utility(
                observation_with_balances({1, 0}, {maximum - 10, 0}),
                {1, 5, 0});
            EXPECT_EQ(value.total, maximum - 5);
            EXPECT_THROW(
                static_cast<void>(
                    calculate_agent_utility_delta(-maximum, maximum)),
                std::overflow_error);
            EXPECT_THROW(
                static_cast<void>(
                    calculate_agent_utility_delta(maximum, -maximum)),
                std::overflow_error);
        }
    }  // namespace
}  // namespace exchange
