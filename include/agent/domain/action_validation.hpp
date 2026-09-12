#pragma once

#include "accounting/financial_conversion.hpp"
#include "agent/domain/agent.hpp"

namespace exchange {
    enum class AgentActionValidationResult {
        Valid,
        InvalidSide,
        InvalidPrice,
        InvalidQuantity,
        InvalidOrderId,
        CancelTargetNotActive,
        InvalidFinancialValue,
    };

    [[nodiscard]] AgentActionValidationResult validate_agent_action(
        const AgentAction& action,
        const AgentObservation& observation,
        const InstrumentContext& instrument);
}  // namespace exchange
