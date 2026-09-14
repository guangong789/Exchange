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

        TEST(AgentActionValidationTest, ValidatesContractActionShape) {
            AgentObservation observation;
            observation.agent_id = 101;
            const ProposeContractAction valid{
                202,
                ContractTerms{
                    101,
                    202,
                    500,
                    ResourceKind::ComputeCredit,
                    10}};
            EXPECT_EQ(
                validate_agent_action(valid, observation, instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(
                    AcceptContractAction{1},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(
                    RejectContractAction{1},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(
                    FulfillResourceObligationAction{1},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                validate_agent_action(
                    SettlePaymentObligationAction{1},
                    observation,
                    instrument),
                AgentActionValidationResult::Valid);

            ProposeContractAction invalid = valid;
            invalid.counterparty = 0;
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractCounterparty);

            invalid = valid;
            invalid.counterparty = 101;
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractCounterparty);

            invalid = valid;
            invalid.terms = ContractTerms{
                202,
                303,
                500,
                ResourceKind::ComputeCredit,
                10};
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractParties);

            invalid = valid;
            invalid.terms.quote_payment_amount = 0;
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractPayment);

            invalid = valid;
            invalid.terms.resource = static_cast<ResourceKind>(99);
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractResource);

            invalid = valid;
            invalid.terms.resource_quantity = 0;
            EXPECT_EQ(
                validate_agent_action(invalid, observation, instrument),
                AgentActionValidationResult::InvalidContractQuantity);

            EXPECT_EQ(
                validate_agent_action(
                    AcceptContractAction{0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    RejectContractAction{0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    FulfillResourceObligationAction{0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    SettlePaymentObligationAction{0},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    AcceptContractAction{
                        std::numeric_limits<ContractId>::max()},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    FulfillResourceObligationAction{
                        std::numeric_limits<ContractId>::max()},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
            EXPECT_EQ(
                validate_agent_action(
                    SettlePaymentObligationAction{
                        std::numeric_limits<ContractId>::max()},
                    observation,
                    instrument),
                AgentActionValidationResult::InvalidContractId);
        }
    }  // namespace
}  // namespace exchange
