#include "agent/runtime/agent_experiment.hpp"

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        AgentTurnRecord submit_turn(
            AgentId agent_id,
            Side side,
            Quantity quantity,
            AgentTurnStatus status,
            AgentSubmitStatus submit_status) {
            AgentTurnRecord turn;
            turn.step = 1;
            turn.local_timestamp_ms = 1'000;
            turn.agent_id = agent_id;
            turn.action = SubmitOrderAction{side, 100, quantity};
            turn.validation = AgentActionValidationResult::Valid;
            turn.economic_constraint =
                AgentEconomicConstraintResult::Allowed;
            turn.execution_result = SubmitActionResult{
                submit_status == AgentSubmitStatus::Accepted ? 1U : 0U,
                submit_status == AgentSubmitStatus::Accepted ? 1 : 0,
                submit_status};
            turn.status = status;
            turn.post_state = AgentStateSummary{
                Balance{quantity, 0},
                Balance{1'000, 0},
                1};
            return turn;
        }

        TEST(AgentExperimentMetricsTest,
             KeepsPerAgentCountersIsolatedAndSumsSocietyTotals) {
            AgentExperimentMetrics metrics;
            metrics.record(submit_turn(
                101,
                Side::Buy,
                2,
                AgentTurnStatus::Executed,
                AgentSubmitStatus::Accepted));
            metrics.record(submit_turn(
                101,
                Side::Sell,
                3,
                AgentTurnStatus::ExecutionRejected,
                AgentSubmitStatus::InsufficientFunds));

            AgentTurnRecord held;
            held.step = 1;
            held.agent_id = 202;
            held.action = HoldAction{};
            held.validation = AgentActionValidationResult::Valid;
            held.economic_constraint =
                AgentEconomicConstraintResult::Allowed;
            held.execution_result = HoldActionResult{};
            held.status = AgentTurnStatus::Held;
            held.post_state = AgentStateSummary{
                Balance{7, 1},
                Balance{900, 100},
                4};
            metrics.record(held);

            AgentTurnRecord cancelled;
            cancelled.step = 2;
            cancelled.agent_id = 202;
            cancelled.action = CancelOrderAction{42};
            cancelled.validation = AgentActionValidationResult::Valid;
            cancelled.economic_constraint =
                AgentEconomicConstraintResult::Allowed;
            cancelled.execution_result = CancelActionResult{
                AgentCancelStatus::Cancelled};
            cancelled.status = AgentTurnStatus::Executed;
            cancelled.post_state = held.post_state;
            metrics.record(cancelled);

            const PerAgentExperimentMetrics* first =
                metrics.find_agent(101);
            ASSERT_NE(first, nullptr);
            EXPECT_EQ(first->turns, 2U);
            EXPECT_EQ(first->proposed_submits, 2U);
            EXPECT_EQ(first->proposed_buys, 1U);
            EXPECT_EQ(first->proposed_sells, 1U);
            EXPECT_EQ(first->total_proposed_quantity, 5);
            EXPECT_EQ(first->total_accepted_quantity, 2);
            EXPECT_EQ(first->successful_executions, 1U);
            EXPECT_EQ(first->execution_rejections, 1U);
            EXPECT_EQ(first->holds, 0U);

            const PerAgentExperimentMetrics* second =
                metrics.find_agent(202);
            ASSERT_NE(second, nullptr);
            EXPECT_EQ(second->turns, 2U);
            EXPECT_EQ(second->proposed_submits, 0U);
            EXPECT_EQ(second->proposed_cancels, 1U);
            EXPECT_EQ(second->holds, 1U);
            EXPECT_EQ(second->successful_executions, 1U);
            EXPECT_EQ(second->final_state, held.post_state);

            EXPECT_EQ(metrics.per_agent().size(), 2U);
            EXPECT_EQ(
                metrics.society(),
                (SocietyExperimentMetrics{
                    4,
                    2,
                    0,
                    0,
                    1,
                    1,
                    1,
                    1}));
            EXPECT_EQ(metrics.find_agent(999), nullptr);
        }

        TEST(AgentExperimentMetricsTest,
             CountsEachRejectionStageWithoutCollapsingOutcomes) {
            AgentExperimentMetrics metrics;

            AgentTurnRecord decision_failed;
            decision_failed.agent_id = 101;
            decision_failed.status = AgentTurnStatus::DecisionFailed;
            metrics.record(decision_failed);

            AgentTurnRecord structural;
            structural.agent_id = 101;
            structural.action = SubmitOrderAction{Side::Buy, 0, 1};
            structural.validation =
                AgentActionValidationResult::InvalidPrice;
            structural.status =
                AgentTurnStatus::StructuralValidationRejected;
            metrics.record(structural);

            AgentTurnRecord economic;
            economic.agent_id = 101;
            economic.action = SubmitOrderAction{Side::Buy, 101, 1};
            economic.validation = AgentActionValidationResult::Valid;
            economic.economic_constraint =
                AgentEconomicConstraintResult::BuyPriceExceeded;
            economic.status = AgentTurnStatus::EconomicConstraintRejected;
            metrics.record(economic);

            const PerAgentExperimentMetrics* agent =
                metrics.find_agent(101);
            ASSERT_NE(agent, nullptr);
            EXPECT_EQ(agent->turns, 3U);
            EXPECT_EQ(agent->decision_failures, 1U);
            EXPECT_EQ(agent->structural_rejections, 1U);
            EXPECT_EQ(agent->economic_constraint_rejections, 1U);
            EXPECT_EQ(agent->execution_rejections, 0U);
            EXPECT_EQ(agent->successful_executions, 0U);
            EXPECT_EQ(metrics.society().total_structural_rejections, 1U);
            EXPECT_EQ(
                metrics.society().total_economic_constraint_rejections,
                1U);
        }

        TEST(AgentExperimentMetricsTest,
             AccumulatesUtilityDeltasAndTracksExtrema) {
            AgentExperimentMetrics metrics;
            for (const UtilityValue delta : {5, -3, 0}) {
                AgentTurnRecord turn;
                turn.agent_id = 101;
                turn.action = HoldAction{};
                turn.status = AgentTurnStatus::Held;
                turn.utility_before = AgentUtilityBreakdown{0, 0, 0, 100};
                turn.utility_after =
                    AgentUtilityBreakdown{0, 0, 0, 100 + delta};
                turn.utility_delta = delta;
                metrics.record(turn);
            }

            const PerAgentExperimentMetrics* agent =
                metrics.find_agent(101);
            ASSERT_NE(agent, nullptr);
            EXPECT_EQ(agent->cumulative_utility_delta, 2);
            EXPECT_EQ(agent->positive_utility_turns, 1U);
            EXPECT_EQ(agent->negative_utility_turns, 1U);
            EXPECT_EQ(agent->zero_utility_turns, 1U);
            EXPECT_EQ(agent->final_utility, 100);
            EXPECT_EQ(agent->best_utility_delta, 5);
            EXPECT_EQ(agent->worst_utility_delta, -3);
        }
    }  // namespace
}  // namespace exchange
