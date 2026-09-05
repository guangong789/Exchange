#pragma once

#include <cstdint>

#include <vector>

#include "exchange/agent/agent.hpp"
#include "exchange/agent/agent_action_gateway.hpp"
#include "exchange/agent/agent_observation_service.hpp"
#include "exchange/agent/agent_registry.hpp"
#include "exchange/arena/arena_scenario.hpp"

namespace exchange {
    struct ArenaParticipant {
        AgentId agent_id{};
        // The policy must outlive the Arena.
        const AgentPolicy* policy{};
    };

    struct ArenaTurnRecord {
        std::uint64_t round{};
        AgentId agent_id{};
        AgentObservation observation;
        AgentAction action;
        AgentActionResult result;

        bool operator==(const ArenaTurnRecord&) const = default;
    };

    struct ArenaAgentOutcome {
        AgentId agent_id{};
        ObjectiveProgress objective;

        bool operator==(const ArenaAgentOutcome&) const = default;
    };

    class Arena {
    public:
        Arena(
            ArenaScenario scenario,
            std::vector<ArenaParticipant> participants,
            const AgentRegistry& registry,
            const AgentObservationService& observation_service,
            AgentActionGateway& action_gateway);

        Arena(const Arena&) = delete;
        Arena& operator=(const Arena&) = delete;
        Arena(Arena&&) = delete;
        Arena& operator=(Arena&&) = delete;

        void run();

        [[nodiscard]] std::uint64_t current_round() const noexcept;
        [[nodiscard]] const std::vector<ArenaTurnRecord>& trace()
            const noexcept;
        [[nodiscard]] const std::vector<ArenaAgentOutcome>& outcomes()
            const noexcept;

    private:
        const ArenaScenario scenario_;
        const std::vector<ArenaParticipant> participants_;
        std::vector<AssetTargetObjective> participant_objectives_;
        const AgentObservationService& observation_service_;
        AgentActionGateway& action_gateway_;
        std::vector<ArenaTurnRecord> trace_;
        std::vector<ArenaAgentOutcome> outcomes_;
        std::uint64_t current_round_{};
        bool run_called_{};
    };
}  // namespace exchange
