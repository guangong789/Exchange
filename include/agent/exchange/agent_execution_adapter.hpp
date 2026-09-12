#pragma once

#include "agent/domain/agent.hpp"

namespace exchange {
    class AgentExecutionAdapter {
    public:
        virtual ~AgentExecutionAdapter() = default;

        [[nodiscard]] virtual AgentActionResult execute(
            AgentId agent_id,
            const AgentAction& action) = 0;
    };
}  // namespace exchange
