#include "exchange/agent.hpp"

#include <stdexcept>

namespace exchange {
    ThresholdBuyPolicy::ThresholdBuyPolicy(
        Price maximum_price,
        Quantity quantity)
        : maximum_price_(maximum_price), quantity_(quantity) {
        if (maximum_price_ <= 0) {
            throw std::invalid_argument(
                "Agent policy maximum price must be positive");
        }
        if (quantity_ <= 0) {
            throw std::invalid_argument(
                "Agent policy quantity must be positive");
        }
    }

    AgentAction ThresholdBuyPolicy::decide(
        const AgentObservation& observation) const {
        if (observation.best_ask.has_value()
            && *observation.best_ask <= maximum_price_) {
            return SubmitOrderAction{
                Side::Buy,
                *observation.best_ask,
                quantity_};
        }
        return HoldAction{};
    }

    AcquireAssetPolicy::AcquireAssetPolicy(Quantity quantity)
        : quantity_(quantity) {
        if (quantity_ <= 0) {
            throw std::invalid_argument(
                "Agent policy quantity must be positive");
        }
    }

    AgentAction AcquireAssetPolicy::decide(
        const AgentObservation& observation) const {
        if (!observation.objective.has_value()) {
            throw std::logic_error(
                "AcquireAssetPolicy requires objective progress");
        }
        if (observation.objective->achieved) {
            return HoldAction{};
        }
        if (!observation.best_ask.has_value()) {
            return HoldAction{};
        }
        return SubmitOrderAction{
            Side::Buy,
            *observation.best_ask,
            quantity_};
    }
}  // namespace exchange
