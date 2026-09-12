#include "agent/domain/action_validation.hpp"

#include <limits>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};

        TEST(AgentActionValidationTest, AcceptsTypedValidActions) {
            AgentObservation observation;
            observation.active_orders.push_back(
                ObservedOrder{9, Side::Buy, 100, 2});

            EXPECT_EQ(
                validate_agent_action(
                    SubmitOrderAction{Side::Buy, 100, 2},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(
                    CancelOrderAction{9},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(HoldAction{}, observation, instrument),
                AgentActionValidationResult::Valid);
        }

        TEST(AgentActionValidationTest, RejectsInvalidActionShape) {
            const AgentObservation observation;

            EXPECT_EQ(
                validate_agent_action(
                    SubmitOrderAction{static_cast<Side>(99), 100, 1},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidSide);
            EXPECT_EQ(
                validate_agent_action(
                    SubmitOrderAction{Side::Buy, 0, 1},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidPrice);
            EXPECT_EQ(
                validate_agent_action(
                    SubmitOrderAction{Side::Buy, 100, 0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidQuantity);
            EXPECT_EQ(
                validate_agent_action(
                    CancelOrderAction{0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidOrderId);
            EXPECT_EQ(
                validate_agent_action(
                    CancelOrderAction{9},
                    observation,
                    instrument),
                AgentActionValidationResult::CancelTargetNotActive);
        }

        TEST(AgentActionValidationTest, RejectsFinancialOverflow) {
            EXPECT_EQ(
                validate_agent_action(
                    SubmitOrderAction{
                        Side::Buy,
                        std::numeric_limits<Price>::max(),
                        2},
                    AgentObservation{},
                    instrument),
                AgentActionValidationResult::InvalidFinancialValue);
        }
    }  // namespace
}  // namespace exchange
