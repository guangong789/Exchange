#pragma once

#include "accounting/financial_conversion.hpp"
#include "agent/domain/agent.hpp"
#include "agent/domain/economic_profile.hpp"

namespace exchange {
    enum class AgentEconomicConstraintResult {
        Allowed,
        OrderQuantityExceeded,
        OrderNotionalExceeded,
        BasePositionExceeded,
        BuyPriceExceeded,
        SellPriceBelowMinimum,
    };

    void validate_agent_economic_profile(
        const AgentEconomicProfile& profile);

    [[nodiscard]] AgentEconomicConstraintResult
        evaluate_agent_economic_constraints(
            const AgentAction& action,
            const AgentObservation& observation,
            const AgentEconomicProfile& profile,
            const InstrumentContext& instrument);
}  // namespace exchange
