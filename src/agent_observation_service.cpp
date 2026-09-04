#include "exchange/agent_observation_service.hpp"

#include <stdexcept>

namespace exchange {
    AgentObservationService::AgentObservationService(
        const AgentRegistry& registry,
        const AccountStore& accounts,
        const OrderBook& order_book,
        InstrumentContext instrument)
        : registry_(registry),
          accounts_(accounts),
          order_book_(order_book),
          instrument_(instrument),
          objective_evaluator_(registry, accounts) {
        validate_instrument_context(instrument_);
    }

    AgentObservation AgentObservationService::observe(
        AgentId agent_id) const {
        const auto identity = registry_.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("Agent does not exist");
        }

        return AgentObservation{
            identity->agent_id,
            identity->account_id,
            order_book_.best_bid(),
            order_book_.best_ask(),
            accounts_.find_balance(
                identity->account_id,
                instrument_.base_asset),
            accounts_.find_balance(
                identity->account_id,
                instrument_.quote_asset),
            std::nullopt,
            std::nullopt,
        };
    }

    AgentObservation AgentObservationService::observe(
        AgentId agent_id,
        const AssetTargetObjective& objective) const {
        AgentObservation observation = observe(agent_id);
        observation.objective = objective_evaluator_.evaluate(
            agent_id,
            objective);
        return observation;
    }
}  // namespace exchange
