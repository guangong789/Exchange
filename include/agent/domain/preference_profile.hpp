#pragma once

#include "accounting/account.hpp"

namespace exchange {
    struct AgentPreferenceProfile {
        Amount target_base_inventory{};
        Amount base_unit_value{};
        Amount inventory_deviation_penalty_per_unit{};

        bool operator==(const AgentPreferenceProfile&) const = default;
    };
}  // namespace exchange
