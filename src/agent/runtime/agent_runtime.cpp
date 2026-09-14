#include "agent/runtime/agent_runtime.hpp"

#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace exchange {
    namespace {
        bool execution_succeeded(const AgentActionResult& result) {
            return std::visit(
                [](const auto& value) {
                    using Result = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Result, HoldActionResult>) {
                        return true;
                    } else if constexpr (std::is_same_v<
                                             Result,
                                             SubmitActionResult>) {
                        return value.status == AgentSubmitStatus::Accepted;
                    } else if constexpr (std::is_same_v<
                                             Result,
                                             CancelActionResult>) {
                        return value.status == AgentCancelStatus::Cancelled;
                    } else {
                        static_assert(std::is_same_v<
                                      Result,
                                      ContractActionResult>);
                        return value.status == ContractResult::Success;
                    }
                },
                result);
        }

        AgentStateSummary summarize_state(
            const AgentObservation& observation) {
            if (observation.active_orders.size()
                > std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error(
                    "Agent active order count overflow");
            }
            return AgentStateSummary{
                observation.base_balance,
                observation.quote_balance,
                static_cast<std::uint64_t>(
                    observation.active_orders.size())};
        }

        std::optional<AgentUtilityBreakdown> evaluate_utility(
            const AgentObservation& observation,
            const std::optional<AgentPreferenceProfile>& preference) {
            if (!preference.has_value()) {
                return std::nullopt;
            }
            return evaluate_agent_utility(observation, *preference);
        }

        void set_unchanged_utility(
            AgentTurnRecord& turn,
            const std::optional<AgentUtilityBreakdown>& utility) {
            turn.utility_before = utility;
            turn.utility_after = utility;
            if (utility.has_value()) {
                turn.utility_delta = 0;
            }
        }

        std::optional<ContractState> contract_state_after_action(
            const AgentAction& action,
            const AgentActionResult& result,
            const AgentObservation& post_observation) {
            std::optional<ContractId> contract_id;
            if (const auto* contract_result = std::get_if<
                    ContractActionResult>(&result)) {
                contract_id = contract_result->contract_id;
            }
            if (!contract_id.has_value()) {
                std::visit(
                    [&contract_id](const auto& payload) {
                        using Action = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<
                                          Action,
                                          AcceptContractAction>
                                      || std::is_same_v<
                                          Action,
                                          RejectContractAction>
                                      || std::is_same_v<
                                          Action,
                                          FulfillResourceObligationAction>
                                      || std::is_same_v<
                                          Action,
                                          SettlePaymentObligationAction>) {
                            contract_id = payload.contract_id;
                        }
                    },
                    action);
            }
            if (!contract_id.has_value()) {
                return std::nullopt;
            }
            const auto contract = std::find_if(
                post_observation.contracts.begin(),
                post_observation.contracts.end(),
                [contract_id](const Contract& contract) {
                    return contract.id == *contract_id;
                });
            return contract == post_observation.contracts.end()
                ? std::nullopt
                : std::optional<ContractState>{contract->state};
        }
    }  // namespace

    AgentRuntime::AgentRuntime(
        std::vector<AgentRuntimeParticipant> participants,
        const AgentObservationService& observation_service,
        AgentExecutionAdapter& execution_adapter,
        InstrumentContext instrument,
        const ExternalMarketFeed* external_market_feed)
        : participants_(std::move(participants)),
          observation_service_(observation_service),
          execution_adapter_(execution_adapter),
          instrument_(instrument),
          external_market_feed_(external_market_feed) {
        validate_instrument_context(instrument_);
        if (participants_.empty()) {
            throw std::invalid_argument(
                "Agent runtime requires at least one participant");
        }

        std::set<AgentId> agent_ids;
        for (const AgentRuntimeParticipant& participant : participants_) {
            if (participant.agent_id == 0) {
                throw std::invalid_argument(
                    "Agent runtime participant ID must be non-zero");
            }
            if (participant.decision_provider == nullptr) {
                throw std::invalid_argument(
                    "Agent runtime decision provider must be non-null");
            }
            if (!agent_ids.insert(participant.agent_id).second) {
                throw std::invalid_argument(
                    "Agent runtime participant IDs must be unique");
            }
            validate_agent_economic_profile(
                participant.economic_profile);
            if (participant.preference_profile.has_value()) {
                validate_agent_preference_profile(
                    *participant.preference_profile);
            }
        }
    }

    void AgentRuntime::run_step() {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        run_step_at(now.count());
    }

    void AgentRuntime::run_step_at(std::int64_t local_now_ms) {
        if (local_now_ms < 0) {
            throw std::invalid_argument(
                "Agent runtime local timestamp must be non-negative");
        }
        if (current_step_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Agent runtime step overflow");
        }
        ++current_step_;

        for (const AgentRuntimeParticipant& participant : participants_) {
            std::optional<ExternalMarketState> external_market_context;
            std::optional<ExternalMarketState> external_market;
            if (external_market_feed_ != nullptr) {
                ExternalMarketState state =
                    external_market_feed_->latest(local_now_ms);
                external_market_context = state;
                if (state.freshness == ExternalMarketFreshness::Fresh) {
                    external_market = std::move(state);
                }
            }
            const WorldState world = observation_service_.capture_world(
                current_step_,
                external_market);
            AgentObservation observation = observation_service_.observe(
                participant.agent_id,
                world,
                participant.objective);
            observation.economic_profile = participant.economic_profile;
            observation.preference_profile = participant.preference_profile;
            const AgentStateSummary pre_state = summarize_state(observation);
            const std::optional<AgentUtilityBreakdown> utility_before =
                evaluate_utility(
                    observation,
                    participant.preference_profile);

            AgentAction action;
            try {
                action = participant.decision_provider->decide(observation);
            } catch (const AgentDecisionError&) {
                AgentTurnRecord turn;
                turn.step = current_step_;
                turn.local_timestamp_ms = local_now_ms;
                turn.agent_id = participant.agent_id;
                turn.observation = std::move(observation);
                turn.external_market_context =
                    std::move(external_market_context);
                turn.pre_state = pre_state;
                turn.post_state = pre_state;
                set_unchanged_utility(turn, utility_before);
                turn.status = AgentTurnStatus::DecisionFailed;
                record_turn(std::move(turn));
                continue;
            }

            const AgentActionValidationResult validation =
                validate_agent_action(action, observation, instrument_);
            if (validation != AgentActionValidationResult::Valid) {
                AgentTurnRecord turn;
                turn.step = current_step_;
                turn.local_timestamp_ms = local_now_ms;
                turn.agent_id = participant.agent_id;
                turn.observation = std::move(observation);
                turn.external_market_context =
                    std::move(external_market_context);
                turn.pre_state = pre_state;
                turn.post_state = pre_state;
                turn.action = std::move(action);
                turn.validation = validation;
                set_unchanged_utility(turn, utility_before);
                turn.status =
                    AgentTurnStatus::StructuralValidationRejected;
                record_turn(std::move(turn));
                continue;
            }

            const AgentEconomicConstraintResult economic_constraint =
                evaluate_agent_economic_constraints(
                    action,
                    observation,
                    participant.economic_profile,
                    instrument_);
            if (economic_constraint
                != AgentEconomicConstraintResult::Allowed) {
                AgentTurnRecord turn;
                turn.step = current_step_;
                turn.local_timestamp_ms = local_now_ms;
                turn.agent_id = participant.agent_id;
                turn.observation = std::move(observation);
                turn.external_market_context =
                    std::move(external_market_context);
                turn.pre_state = pre_state;
                turn.post_state = pre_state;
                turn.action = std::move(action);
                turn.validation = validation;
                turn.economic_constraint = economic_constraint;
                set_unchanged_utility(turn, utility_before);
                turn.status =
                    AgentTurnStatus::EconomicConstraintRejected;
                record_turn(std::move(turn));
                continue;
            }

            AgentActionResult result = execution_adapter_.execute(
                participant.agent_id,
                action);
            const WorldState post_world =
                observation_service_.capture_world(
                    current_step_,
                    external_market);
            AgentObservation post_observation =
                observation_service_.observe(
                    participant.agent_id,
                    post_world,
                    participant.objective);
            post_observation.economic_profile =
                participant.economic_profile;
            post_observation.preference_profile =
                participant.preference_profile;
            const std::optional<AgentUtilityBreakdown> utility_after =
                evaluate_utility(
                    post_observation,
                    participant.preference_profile);

            AgentTurnRecord turn;
            turn.step = current_step_;
            turn.local_timestamp_ms = local_now_ms;
            turn.agent_id = participant.agent_id;
            turn.observation = std::move(observation);
            turn.external_market_context =
                std::move(external_market_context);
            turn.pre_state = pre_state;
            turn.post_state = summarize_state(post_observation);
            turn.action = std::move(action);
            turn.validation = validation;
            turn.economic_constraint = economic_constraint;
            turn.execution_result = std::move(result);
            turn.contract_state_after_action = contract_state_after_action(
                *turn.action,
                *turn.execution_result,
                post_observation);
            turn.utility_before = utility_before;
            turn.utility_after = utility_after;
            if (utility_before.has_value()) {
                turn.utility_delta = calculate_agent_utility_delta(
                    utility_before->total,
                    utility_after->total);
            }
            if (std::holds_alternative<HoldAction>(*turn.action)) {
                turn.status = AgentTurnStatus::Held;
            } else {
                turn.status = execution_succeeded(*turn.execution_result)
                    ? AgentTurnStatus::Executed
                    : AgentTurnStatus::ExecutionRejected;
            }
            record_turn(std::move(turn));
        }
    }

    std::uint64_t AgentRuntime::current_step() const noexcept {
        return current_step_;
    }

    const std::vector<AgentTurnRecord>& AgentRuntime::trace()
        const noexcept {
        return trace_;
    }

    const AgentExperimentMetrics& AgentRuntime::metrics()
        const noexcept {
        return metrics_;
    }

    void AgentRuntime::record_turn(AgentTurnRecord turn) {
        trace_.push_back(std::move(turn));
        metrics_.record(trace_.back());
    }
}  // namespace exchange
