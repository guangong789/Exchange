#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "agent/domain/agent.hpp"

namespace exchange {
    struct DeepSeekPrompt {
        std::string system;
        std::string user;

        bool operator==(const DeepSeekPrompt&) const = default;
    };

    class DeepSeekDecisionError : public AgentDecisionError {
    public:
        using AgentDecisionError::AgentDecisionError;
    };

    [[nodiscard]] DeepSeekPrompt build_deepseek_prompt(
        const AgentObservation& observation);

    [[nodiscard]] AgentAction parse_deepseek_action(
        std::string_view content);

    class DeepSeekDecisionProvider final : public AgentDecisionProvider {
    public:
        using CompletionFunction =
            std::function<std::string(const DeepSeekPrompt&)>;

        explicit DeepSeekDecisionProvider(CompletionFunction complete);

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        CompletionFunction complete_;
    };
}  // namespace exchange
