#pragma once

#include "accounting/account_store.hpp"
#include "accounting/order_reservation_store.hpp"
#include "agent/domain/agent.hpp"
#include "agent/domain/contract_store.hpp"
#include "agent/exchange/agent_objective.hpp"
#include "agent/domain/agent_registry.hpp"
#include "accounting/financial_conversion.hpp"
#include "matching/order_book.hpp"

namespace exchange {
    class AgentObservationService {
    public:
        AgentObservationService(
            const AgentRegistry& registry,
            const AccountStore& accounts,
            const OrderReservationStore& reservations,
            const OrderBook& order_book,
            const ContractStore& contracts,
            InstrumentContext instrument);

        [[nodiscard]] WorldState capture_world(
            std::uint64_t step,
            std::optional<ExternalMarketState> external_market =
                std::nullopt) const;

        [[nodiscard]] AgentObservation observe(
            AgentId agent_id,
            const WorldState& world,
            std::optional<AssetTargetObjective> objective = std::nullopt) const;

    private:
        const AgentRegistry& registry_;
        const AccountStore& accounts_;
        const OrderReservationStore& reservations_;
        const OrderBook& order_book_;
        const ContractStore& contracts_;
        const InstrumentContext instrument_;
        ObjectiveEvaluator objective_evaluator_;
    };
}  // namespace exchange
