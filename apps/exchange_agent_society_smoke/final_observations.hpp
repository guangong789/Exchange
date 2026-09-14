#pragma once

#include <map>
#include <span>
#include <utility>

#include "agent/runtime/agent_runtime.hpp"

namespace exchange::society_smoke {
    // Call after all participants have finished; turn snapshots are historical.
    inline std::map<AgentId, AgentObservation> capture_final_observations(
        const AgentObservationService& observations,
        std::span<const AgentRuntimeParticipant> participants,
        std::uint64_t step) {
        const WorldState world = observations.capture_world(step);
        std::map<AgentId, AgentObservation> result;
        for (const auto& participant : participants) {
            auto observation = observations.observe(
                participant.agent_id, world, participant.objective);
            observation.economic_profile = participant.economic_profile;
            observation.preference_profile = participant.preference_profile;
            result.emplace(participant.agent_id, std::move(observation));
        }
        return result;
    }
}  // namespace exchange::society_smoke
