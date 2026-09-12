#include "agent/runtime/agent_experiment.hpp"

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
                    } else {
                        increment(metrics.holds);
                    }
                },
                *turn.action);
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
                break;
            case AgentTurnStatus::Executed:
                increment(metrics.successful_executions);
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
            add_counter(result.total_holds, agent.holds);
        }
        return result;
    }
}  // namespace exchange
