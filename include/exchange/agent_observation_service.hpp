#pragma once

#include "exchange/account_store.hpp"
#include "exchange/agent.hpp"
#include "exchange/agent_objective.hpp"
#include "exchange/agent_registry.hpp"
#include "exchange/financial_conversion.hpp"
#include "exchange/order_book.hpp"

namespace exchange {
    class AgentObservationService {
    public:
        AgentObservationService(
            const AgentRegistry& registry,
            const AccountStore& accounts,
            const OrderBook& order_book,
            InstrumentContext instrument);

        [[nodiscard]] AgentObservation observe(AgentId agent_id) const;
        [[nodiscard]] AgentObservation observe(
            AgentId agent_id,
            const AssetTargetObjective& objective) const;

    private:
        const AgentRegistry& registry_;
        const AccountStore& accounts_;
        const OrderBook& order_book_;
        const InstrumentContext instrument_;
        ObjectiveEvaluator objective_evaluator_;
    };
}  // namespace exchange
