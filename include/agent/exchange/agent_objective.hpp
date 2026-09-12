#pragma once

#include "accounting/account_store.hpp"
#include "agent/domain/agent.hpp"
#include "agent/domain/agent_registry.hpp"

namespace exchange {
    class ObjectiveEvaluator {
    public:
        ObjectiveEvaluator(
            const AgentRegistry& registry,
            const AccountStore& accounts) noexcept;

        [[nodiscard]] ObjectiveProgress evaluate(
            AgentId agent_id,
            const AssetTargetObjective& objective) const;

    private:
        const AgentRegistry& registry_;
        const AccountStore& accounts_;
    };
}  // namespace exchange
