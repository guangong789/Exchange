#pragma once

#include "agent/domain/agent.hpp"

namespace exchange {
    class ThresholdBuyPolicy final : public AgentDecisionProvider {
    public:
        ThresholdBuyPolicy(Price maximum_price, Quantity quantity);

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        Price maximum_price_;
        Quantity quantity_;
    };

    class AcquireAssetPolicy final : public AgentDecisionProvider {
    public:
        explicit AcquireAssetPolicy(Quantity quantity);

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        Quantity quantity_;
    };
}  // namespace exchange
