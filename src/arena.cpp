#include "exchange/arena.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace exchange {
    Arena::Arena(
        ArenaScenario scenario,
        std::vector<ArenaParticipant> participants,
        const AgentRegistry& registry,
        const AgentObservationService& observation_service,
        AgentActionGateway& action_gateway)
        : scenario_(std::move(scenario)),
          participants_(std::move(participants)),
          observation_service_(observation_service),
          action_gateway_(action_gateway) {
        if (scenario_.config.max_rounds == 0) {
            throw std::invalid_argument(
                "Arena max rounds must be positive");
        }
        if (participants_.empty()) {
            throw std::invalid_argument(
                "Arena must contain at least one participant");
        }

        std::set<AgentId> participant_ids;
        for (const ArenaParticipant& participant : participants_) {
            if (participant.agent_id == 0) {
                throw std::invalid_argument(
                    "Arena participant Agent ID must be non-zero");
            }
            if (participant.policy == nullptr) {
                throw std::invalid_argument(
                    "Arena participant policy must be non-null");
            }
            if (!participant_ids.insert(participant.agent_id).second) {
                throw std::invalid_argument(
                    "Arena participant Agent ID must be unique");
            }
            if (!registry.find(participant.agent_id).has_value()) {
                throw std::out_of_range(
                    "Arena participant Agent does not exist");
            }
        }

        std::set<AgentId> objective_agent_ids;
        for (const AgentObjectiveAssignment& assignment :
             scenario_.objectives) {
            if (assignment.agent_id == 0) {
                throw std::invalid_argument(
                    "Objective Agent ID must be non-zero");
            }
            if (assignment.objective.asset_id == 0) {
                throw std::invalid_argument(
                    "Objective Asset ID must be non-zero");
            }
            if (assignment.objective.target_amount <= 0) {
                throw std::invalid_argument(
                    "Objective target amount must be positive");
            }
            if (!objective_agent_ids.insert(assignment.agent_id).second) {
                throw std::invalid_argument(
                    "Arena objective Agent ID must be unique");
            }
            if (!registry.find(assignment.agent_id).has_value()) {
                throw std::out_of_range(
                    "Arena objective Agent does not exist");
            }
            if (!participant_ids.contains(assignment.agent_id)) {
                throw std::invalid_argument(
                    "Arena objective Agent is not a participant");
            }
        }

        participant_objectives_.reserve(participants_.size());
        for (const ArenaParticipant& participant : participants_) {
            const auto assignment = std::find_if(
                scenario_.objectives.begin(),
                scenario_.objectives.end(),
                [&](const AgentObjectiveAssignment& candidate) {
                    return candidate.agent_id == participant.agent_id;
                });
            if (assignment == scenario_.objectives.end()) {
                throw std::invalid_argument(
                    "Arena participant is missing an objective");
            }
            participant_objectives_.push_back(assignment->objective);
        }

        const std::uint64_t participant_count =
            static_cast<std::uint64_t>(participants_.size());
        if (scenario_.config.max_rounds
            > std::numeric_limits<std::size_t>::max()
                  / participant_count) {
            throw std::length_error("Arena trace size overflow");
        }
        trace_.reserve(
            static_cast<std::size_t>(
                scenario_.config.max_rounds * participant_count));
        outcomes_.reserve(participants_.size());
    }

    void Arena::run() {
        if (run_called_) {
            throw std::logic_error("Arena is one-shot");
        }
        run_called_ = true;

        for (std::uint64_t round = 1;; ++round) {
            current_round_ = round;
            for (std::size_t participant_index = 0;
                 participant_index < participants_.size();
                 ++participant_index) {
                const ArenaParticipant& participant =
                    participants_[participant_index];
                AgentObservation observation =
                    observation_service_.observe(
                        participant.agent_id,
                        participant_objectives_[participant_index]);
                AgentAction action =
                    participant.policy->decide(observation);
                AgentActionResult result = action_gateway_.execute(
                    participant.agent_id,
                    action);
                trace_.push_back(ArenaTurnRecord{
                    round,
                    participant.agent_id,
                    std::move(observation),
                    std::move(action),
                    std::move(result),
                });
            }
            if (round == scenario_.config.max_rounds) {
                break;
            }
        }

        for (std::size_t participant_index = 0;
             participant_index < participants_.size();
             ++participant_index) {
            const AgentId agent_id =
                participants_[participant_index].agent_id;
            const AgentObservation terminal_observation =
                observation_service_.observe(
                    agent_id,
                    participant_objectives_[participant_index]);
            outcomes_.push_back(ArenaAgentOutcome{
                agent_id,
                *terminal_observation.objective});
        }
    }

    std::uint64_t Arena::current_round() const noexcept {
        return current_round_;
    }

    const std::vector<ArenaTurnRecord>& Arena::trace() const noexcept {
        return trace_;
    }

    const std::vector<ArenaAgentOutcome>& Arena::outcomes()
        const noexcept {
        return outcomes_;
    }
}  // namespace exchange
