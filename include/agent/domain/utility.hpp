#pragma once

#include <cstdint>

#include "agent/domain/agent.hpp"
#include "agent/domain/preference_profile.hpp"

namespace exchange {
    using UtilityValue = std::int64_t;

    struct AgentUtilityBreakdown {
        UtilityValue quote_component{};
        UtilityValue base_value_component{};
        UtilityValue inventory_penalty{};
        UtilityValue total{};

        bool operator==(const AgentUtilityBreakdown&) const = default;
    };

    void validate_agent_preference_profile(
        const AgentPreferenceProfile& profile);

    [[nodiscard]] AgentUtilityBreakdown evaluate_agent_utility(
        const AgentObservation& observation,
        const AgentPreferenceProfile& profile);

    [[nodiscard]] UtilityValue calculate_agent_utility_delta(
        UtilityValue before,
        UtilityValue after);
}  // namespace exchange
