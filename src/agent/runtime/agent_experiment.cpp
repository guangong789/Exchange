#include "agent/runtime/agent_experiment.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    namespace {
        void increment(std::uint64_t& value) {
            if (value == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Agent experiment counter overflow");
            }
            ++value;
        }

        void add_quantity(Quantity& total, Quantity quantity) {
            if (quantity < 0) {
                throw std::logic_error(
                    "Agent experiment quantity must be non-negative");
            }
            if (total > std::numeric_limits<Quantity>::max() - quantity) {
                throw std::overflow_error(
                    "Agent experiment quantity overflow");
            }
            total += quantity;
        }

        void add_counter(std::uint64_t& total, std::uint64_t value) {
            if (total > std::numeric_limits<std::uint64_t>::max() - value) {
                throw std::overflow_error("Society metrics counter overflow");
            }
            total += value;
        }

        void add_utility(
            UtilityValue& total,
            UtilityValue delta) {
            if ((delta > 0
                 && total
                     > std::numeric_limits<UtilityValue>::max() - delta)
                || (delta < 0
                    && total
                        < std::numeric_limits<UtilityValue>::min() - delta)) {
                throw std::overflow_error(
                    "Cumulative Agent utility overflow");
            }
            total += delta;
        }

        bool submit_was_accepted(const AgentTurnRecord& turn) {
            if (turn.status != AgentTurnStatus::Executed
                || !turn.execution_result.has_value()) {
                return false;
            }
            const auto* result = std::get_if<SubmitActionResult>(
                &*turn.execution_result);
            return result != nullptr
                && result->status == AgentSubmitStatus::Accepted;
        }
    }  // namespace

    void AgentExperimentMetrics::record(const AgentTurnRecord& turn) {
        auto [iterator, inserted] = per_agent_.try_emplace(turn.agent_id);
        PerAgentExperimentMetrics& metrics = iterator->second;
        if (inserted) {
            metrics.agent_id = turn.agent_id;
        }

        increment(metrics.turns);
        metrics.final_state = turn.post_state;
        if (turn.utility_delta.has_value()) {
            if (!turn.utility_before.has_value()
                || !turn.utility_after.has_value()) {
                throw std::logic_error(
                    "Agent utility delta has no complete evaluation");
            }
            const UtilityValue delta = *turn.utility_delta;
            add_utility(metrics.cumulative_utility_delta, delta);
            metrics.final_utility = turn.utility_after->total;
            if (!metrics.best_utility_delta.has_value()
                || delta > *metrics.best_utility_delta) {
                metrics.best_utility_delta = delta;
            }
            if (!metrics.worst_utility_delta.has_value()
                || delta < *metrics.worst_utility_delta) {
                metrics.worst_utility_delta = delta;
            }
            if (delta > 0) {
                increment(metrics.positive_utility_turns);
            } else if (delta < 0) {
                increment(metrics.negative_utility_turns);
            } else {
                increment(metrics.zero_utility_turns);
            }
        } else if (turn.utility_before.has_value()
                   || turn.utility_after.has_value()) {
            throw std::logic_error(
                "Agent utility evaluation has no delta");
        }

        bool is_contract_action = false;
        if (turn.action.has_value()) {
            std::visit(
                [&](const auto& action) {
                    using Action = std::decay_t<decltype(action)>;
                    if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
                        increment(metrics.proposed_submits);
                        if (action.side == Side::Buy) {
                            increment(metrics.proposed_buys);
                        } else if (action.side == Side::Sell) {
                            increment(metrics.proposed_sells);
                        } else {
                            throw std::logic_error(
                                "Agent experiment has invalid order side");
                        }
                        if (action.quantity > 0) {
                            add_quantity(
                                metrics.total_proposed_quantity,
                                action.quantity);
                        }
                        if (submit_was_accepted(turn)) {
                            add_quantity(
                                metrics.total_accepted_quantity,
                                action.quantity);
                        }
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             CancelOrderAction>) {
                        increment(metrics.proposed_cancels);
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             ProposeContractAction>) {
                        increment(metrics.contract_proposals);
                        is_contract_action = true;
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             AcceptContractAction>) {
                        increment(metrics.contract_accepts);
                        is_contract_action = true;
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             RejectContractAction>) {
                        increment(metrics.contract_rejects);
                        is_contract_action = true;
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             FulfillResourceObligationAction>) {
                        increment(metrics.contract_fulfillment_attempts);
                        is_contract_action = true;
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             SettlePaymentObligationAction>) {
                        increment(metrics.contract_settlement_attempts);
                        is_contract_action = true;
                    } else {
                        static_assert(std::is_same_v<Action, HoldAction>);
                        increment(metrics.holds);
                    }
                },
                *turn.action);

            if (const auto* proposal = std::get_if<ProposeContractAction>(
                    &*turn.action);
                proposal != nullptr && turn.agent_id != 0
                && proposal->counterparty != 0
                && proposal->counterparty != turn.agent_id) {
                increment(contract_pair_interactions_[
                    {turn.agent_id, proposal->counterparty}]
                              .proposal_attempts);
            }
            if (const auto* acceptance = std::get_if<AcceptContractAction>(
                    &*turn.action);
                acceptance != nullptr && turn.execution_result.has_value()) {
                const auto* result = std::get_if<ContractActionResult>(
                    &*turn.execution_result);
                if (result != nullptr
                    && result->status == ContractResult::Success) {
                    const auto contract = std::find_if(
                        turn.observation.contracts.begin(),
                        turn.observation.contracts.end(),
                        [acceptance](const Contract& contract) {
                            return contract.id == acceptance->contract_id;
                        });
                    if (contract != turn.observation.contracts.end()) {
                        increment(contract_pair_interactions_[
                            {contract->proposer, contract->counterparty}]
                                      .accepted_contracts);
                    }
                }
            }
        }

        switch (turn.status) {
            case AgentTurnStatus::DecisionFailed:
                increment(metrics.decision_failures);
                break;
            case AgentTurnStatus::StructuralValidationRejected:
                increment(metrics.structural_rejections);
                break;
            case AgentTurnStatus::EconomicConstraintRejected:
                increment(metrics.economic_constraint_rejections);
                break;
            case AgentTurnStatus::ExecutionRejected:
                increment(metrics.execution_rejections);
                if (is_contract_action) {
                    increment(metrics.contract_execution_rejections);
                    if (turn.action.has_value()
                        && std::holds_alternative<
                            FulfillResourceObligationAction>(*turn.action)) {
                        increment(
                            metrics.contract_fulfillment_rejections);
                    }
                    if (turn.action.has_value()
                        && std::holds_alternative<
                            SettlePaymentObligationAction>(*turn.action)) {
                        increment(metrics.contract_settlement_rejections);
                    }
                }
                break;
            case AgentTurnStatus::Executed:
                increment(metrics.successful_executions);
                if (is_contract_action) {
                    increment(metrics.successful_contract_actions);
                    if (turn.action.has_value()
                        && std::holds_alternative<
                            FulfillResourceObligationAction>(*turn.action)) {
                        increment(
                            metrics.successful_contract_fulfillments);
                    }
                    if (turn.action.has_value()
                        && std::holds_alternative<
                            SettlePaymentObligationAction>(*turn.action)) {
                        increment(metrics.successful_contract_settlements);
                    }
                }
                break;
            case AgentTurnStatus::Held:
                break;
        }
    }

    const PerAgentExperimentMetrics* AgentExperimentMetrics::find_agent(
        AgentId agent_id) const noexcept {
        const auto iterator = per_agent_.find(agent_id);
        return iterator == per_agent_.end() ? nullptr : &iterator->second;
    }

    const std::map<AgentId, PerAgentExperimentMetrics>&
    AgentExperimentMetrics::per_agent() const noexcept {
        return per_agent_;
    }

    const std::map<
        std::pair<AgentId, AgentId>,
        ContractPairInteractionMetrics>&
    AgentExperimentMetrics::contract_pair_interactions() const noexcept {
        return contract_pair_interactions_;
    }

    SocietyExperimentMetrics AgentExperimentMetrics::society() const {
        SocietyExperimentMetrics result;
        for (const auto& [agent_id, agent] : per_agent_) {
            static_cast<void>(agent_id);
            add_counter(result.total_turns, agent.turns);
            add_counter(
                result.total_successful_executions,
                agent.successful_executions);
            add_counter(
                result.total_structural_rejections,
                agent.structural_rejections);
            add_counter(
                result.total_economic_constraint_rejections,
                agent.economic_constraint_rejections);
            add_counter(
                result.total_exchange_rejections,
                agent.execution_rejections);
            add_counter(result.total_buys, agent.proposed_buys);
            add_counter(result.total_sells, agent.proposed_sells);
            add_counter(
                result.total_contract_proposals,
                agent.contract_proposals);
            add_counter(
                result.total_contract_accepts,
                agent.contract_accepts);
            add_counter(
                result.total_contract_rejects,
                agent.contract_rejects);
            add_counter(
                result.total_contract_fulfillment_attempts,
                agent.contract_fulfillment_attempts);
            add_counter(
                result.total_successful_contract_fulfillments,
                agent.successful_contract_fulfillments);
            add_counter(
                result.total_contract_fulfillment_rejections,
                agent.contract_fulfillment_rejections);
            add_counter(
                result.total_contract_settlement_attempts,
                agent.contract_settlement_attempts);
            add_counter(
                result.total_successful_contract_settlements,
                agent.successful_contract_settlements);
            add_counter(
                result.total_contract_settlement_rejections,
                agent.contract_settlement_rejections);
            add_counter(
                result.total_successful_contract_actions,
                agent.successful_contract_actions);
            add_counter(
                result.total_contract_execution_rejections,
                agent.contract_execution_rejections);
            add_counter(result.total_holds, agent.holds);
        }
        return result;
    }
}  // namespace exchange
