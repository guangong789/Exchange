#pragma once

#include "exchange/agent.hpp"
#include "exchange/agent_registry.hpp"
#include "exchange/execution_coordinator.hpp"

namespace exchange {
    class AgentActionGateway {
    public:
        AgentActionGateway(
            const AgentRegistry& registry,
            ExecutionCoordinator& execution_coordinator) noexcept;

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
        OrderId next_order_id_{1};
        Timestamp next_timestamp_{1};
    };
}  // namespace exchange
