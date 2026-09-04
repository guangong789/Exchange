#pragma once

#include "exchange/agent.hpp"
#include "exchange/model_adapter.hpp"

namespace exchange {
    [[nodiscard]] ModelRequest build_model_request(
        const AgentObservation& observation);

    class ModelAgentPolicy final : public AgentPolicy {
    public:
        explicit ModelAgentPolicy(ModelAdapter& adapter) noexcept;

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        ModelAdapter& adapter_;
    };
}  // namespace exchange
