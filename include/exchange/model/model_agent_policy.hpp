#pragma once

#include <string_view>

#include "exchange/agent/agent.hpp"
#include "exchange/model/model_adapter.hpp"

namespace exchange {
    [[nodiscard]] ModelRequest build_model_request(
        const AgentObservation& observation);

    class ModelAgentPolicy final : public AgentPolicy {
    public:
        explicit ModelAgentPolicy(ModelAdapter& adapter) noexcept;

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation,
            std::string_view additional_context) const;

    private:
        ModelAdapter& adapter_;
    };
}  // namespace exchange
