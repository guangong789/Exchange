#include "agent/exchange/agent_observation_service.hpp"

#include <stdexcept>

namespace exchange {
    AgentObservationService::AgentObservationService(
        const AgentRegistry& registry,
        const AccountStore& accounts,
        const OrderReservationStore& reservations,
        const OrderBook& order_book,
        const ContractStore& contracts,
        InstrumentContext instrument)
        : registry_(registry),
          accounts_(accounts),
          reservations_(reservations),
          order_book_(order_book),
          contracts_(contracts),
          instrument_(instrument),
          objective_evaluator_(registry, accounts) {
        validate_instrument_context(instrument_);
    }

    WorldState AgentObservationService::capture_world(
        std::uint64_t step,
        std::optional<ExternalMarketState> external_market) const {
        return WorldState{
            step,
            InternalMarketState{
                order_book_.best_bid(),
                order_book_.best_ask()},
            std::move(external_market),
        };
    }

    AgentObservation AgentObservationService::observe(
        AgentId agent_id,
        const WorldState& world,
        std::optional<AssetTargetObjective> objective) const {
        const auto identity = registry_.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("Agent does not exist");
        }

        AgentObservation observation{
            identity->agent_id,
            identity->account_id,
            world,
            accounts_.find_balance(
                identity->account_id,
                instrument_.base_asset),
            accounts_.find_balance(
                identity->account_id,
                instrument_.quote_asset),
            {},
            std::nullopt,
            {},
            std::nullopt,
            {},
        };

        for (const auto& [order_id, reservation] :
             reservations_.entries()) {
            if (reservation.account_id != identity->account_id) {
                continue;
            }
            const auto order = order_book_.find_order(order_id);
            if (!order.has_value()) {
                throw std::logic_error(
                    "Agent reservation has no active order");
            }
            observation.active_orders.push_back(ObservedOrder{
                order->id,
                order->side,
                order->price,
                order->quantity,
            });
        }

        if (objective.has_value()) {
            observation.objective = objective_evaluator_.evaluate(
                agent_id,
                *objective);
        }
        observation.contracts = contracts_.find_relevant(agent_id);
        return observation;
    }
}  // namespace exchange
