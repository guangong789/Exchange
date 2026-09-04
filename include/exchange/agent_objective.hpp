#pragma once

#include "exchange/account_store.hpp"
#include "exchange/agent.hpp"
#include "exchange/agent_registry.hpp"

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
