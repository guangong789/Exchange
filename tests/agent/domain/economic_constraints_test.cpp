#include "agent/domain/economic_constraints.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};

        AgentEconomicProfile profile_with_limits() {
            AgentEconomicProfile profile;
            profile.max_order_quantity = 5;
            profile.max_order_notional = 500;
            profile.max_base_position = 10;
            profile.max_buy_price = 100;
            profile.min_sell_price = 80;
            return profile;
        }

        AgentEconomicConstraintResult evaluate(
            AgentAction action,
            AgentObservation observation = {},
            AgentEconomicProfile profile = profile_with_limits()) {
            return evaluate_agent_economic_constraints(
                action,
                observation,
                profile,
                instrument);
        }

        TEST(AgentEconomicConstraintsTest,
             QuantityAtLimitIsAllowedAndAboveLimitIsRejected) {
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 90, 5}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 80, 6}),
                AgentEconomicConstraintResult::OrderQuantityExceeded);
        }

        TEST(AgentEconomicConstraintsTest,
             NotionalAtLimitIsAllowedAndAboveLimitIsRejected) {
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 100, 5}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 101, 5}),
                AgentEconomicConstraintResult::OrderNotionalExceeded);
        }

        TEST(AgentEconomicConstraintsTest,
             BuyPriceAtOrBelowMaximumIsAllowedAndAboveIsRejected) {
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 100, 1}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 99, 1}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Buy, 101, 1}),
                AgentEconomicConstraintResult::BuyPriceExceeded);
        }

        TEST(AgentEconomicConstraintsTest,
             SellPriceAtOrAboveMinimumIsAllowedAndBelowIsRejected) {
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Sell, 80, 1}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Sell, 81, 1}),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(SubmitOrderAction{Side::Sell, 79, 1}),
                AgentEconomicConstraintResult::SellPriceBelowMinimum);
        }

        TEST(AgentEconomicConstraintsTest,
             BuyPositionIncludesOwnedBaseAndExistingOpenBuyExposure) {
            AgentObservation observation;
            observation.base_balance = Balance{2, 1};
            observation.active_orders.push_back(
                ObservedOrder{1, Side::Buy, 90, 2});

            EXPECT_EQ(
                evaluate(
                    SubmitOrderAction{Side::Buy, 90, 5},
                    observation),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(
                    SubmitOrderAction{Side::Buy, 90, 6},
                    observation,
                    AgentEconomicProfile{
                        6,
                        1'000,
                        10,
                        100,
                        80}),
                AgentEconomicConstraintResult::BasePositionExceeded);
        }

        TEST(AgentEconomicConstraintsTest,
             HoldAndCancelIgnoreSubmitPriceAndPositionLimits) {
            AgentEconomicProfile profile = profile_with_limits();
            profile.max_base_position = 0;
            profile.max_buy_price = 1;
            profile.min_sell_price = 1'000;
            AgentObservation observation;
            observation.base_balance = Balance{100, 0};

            EXPECT_EQ(
                evaluate(HoldAction{}, observation, profile),
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(
                evaluate(CancelOrderAction{99}, observation, profile),
                AgentEconomicConstraintResult::Allowed);
        }

        TEST(AgentEconomicConstraintsTest,
             InvalidProfilesAndUnrepresentableNotionalDoNotFailOpen) {
            AgentEconomicProfile invalid = profile_with_limits();
            invalid.max_order_quantity = 0;
            EXPECT_THROW(
                static_cast<void>(evaluate(
                    HoldAction{},
                    AgentObservation{},
                    invalid)),
                std::invalid_argument);

            AgentEconomicProfile unlimited;
            EXPECT_THROW(
                static_cast<void>(evaluate(
                    SubmitOrderAction{
                        Side::Buy,
                        std::numeric_limits<Price>::max(),
                        2},
                    AgentObservation{},
                    unlimited)),
                std::overflow_error);
        }
    }  // namespace
}  // namespace exchange
