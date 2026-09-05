#pragma once

#include <cstdint>
#include <vector>

#include "exchange/agent/agent.hpp"

namespace exchange {
    struct ArenaConfig {
        std::uint64_t max_rounds{};

        bool operator==(const ArenaConfig&) const = default;
    };

    struct AgentObjectiveAssignment {
        AgentId agent_id{};
        AssetTargetObjective objective;

        bool operator==(const AgentObjectiveAssignment&) const = default;
    };

    struct ArenaScenario {
        ArenaConfig config;
        std::vector<AgentObjectiveAssignment> objectives;

        bool operator==(const ArenaScenario&) const = default;
    };
}  // namespace exchange
