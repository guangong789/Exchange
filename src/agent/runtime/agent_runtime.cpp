#include "agent/runtime/agent_runtime.hpp"

#include <limits>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

namespace exchange {
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
            std::optional<ExternalMarketState> external_market;
            if (external_market_feed_ != nullptr) {
                ExternalMarketState state =
                    external_market_feed_->latest(local_now_ms);
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

            AgentAction action;
            try {
                action = participant.decision_provider->decide(observation);
            } catch (const AgentDecisionError&) {
                trace_.push_back(AgentTurnRecord{
                    current_step_,
                    participant.agent_id,
                    std::move(observation),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    AgentTurnStatus::DecisionFailed,
                });
                continue;
            }

            const AgentActionValidationResult validation =
                validate_agent_action(action, observation, instrument_);
            if (validation != AgentActionValidationResult::Valid) {
                trace_.push_back(AgentTurnRecord{
                    current_step_,
                    participant.agent_id,
                    std::move(observation),
                    std::move(action),
                    validation,
                    std::nullopt,
                    AgentTurnStatus::ActionRejected,
                });
                continue;
            }

            AgentActionResult result = execution_adapter_.execute(
                participant.agent_id,
                action);
            trace_.push_back(AgentTurnRecord{
                current_step_,
                participant.agent_id,
                std::move(observation),
                std::move(action),
                validation,
                std::move(result),
                AgentTurnStatus::Executed,
            });
        }
    }

    std::uint64_t AgentRuntime::current_step() const noexcept {
        return current_step_;
    }

    const std::vector<AgentTurnRecord>& AgentRuntime::trace()
        const noexcept {
        return trace_;
    }
}  // namespace exchange
