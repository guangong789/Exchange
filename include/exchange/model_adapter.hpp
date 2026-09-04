#pragma once

#include <stdexcept>
#include <string>

namespace exchange {
    struct ModelRequest {
        std::string system_prompt;
        std::string user_prompt;

        bool operator==(const ModelRequest&) const = default;
    };

    struct ModelResponse {
        std::string content;

        bool operator==(const ModelResponse&) const = default;
    };

    class ModelAdapterError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class ModelAdapter {
    public:
        virtual ~ModelAdapter() = default;

        // External inference may be stochastic. Deterministic world behavior
        // begins only after a concrete AgentAction has been produced.
        [[nodiscard]] virtual ModelResponse invoke(
            const ModelRequest& request) = 0;
    };
}  // namespace exchange
