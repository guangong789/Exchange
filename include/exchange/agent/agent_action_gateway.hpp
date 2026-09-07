#pragma once

#include "exchange/core/types.hpp"

#include "exchange/agent/agent.hpp"
#include "exchange/agent/agent_registry.hpp"
#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/execution/execution_sequencer.hpp"

namespace exchange {
    class AgentActionGateway {
    public:
        AgentActionGateway(
            const AgentRegistry& registry,
            ExecutionCoordinator& execution_coordinator,
            ExecutionSequencer& sequencer) noexcept;

        AgentActionGateway(const AgentActionGateway&) = delete;
        AgentActionGateway& operator=(const AgentActionGateway&) = delete;
        AgentActionGateway(AgentActionGateway&&) = delete;
        AgentActionGateway& operator=(AgentActionGateway&&) = delete;

        [[nodiscard]] AgentActionResult execute(
            AgentId agent_id,
            const AgentAction& action);

    private:
        const AgentRegistry& registry_;
        ExecutionCoordinator& execution_coordinator_;
        ExecutionSequencer& sequencer_;
    };
}  // namespace exchange
