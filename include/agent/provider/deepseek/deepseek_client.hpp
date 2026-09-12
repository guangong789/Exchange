#pragma once

#include <chrono>
#include <string>

#include "agent/provider/deepseek/deepseek_decision_provider.hpp"

namespace exchange {
    struct DeepSeekClientConfig {
        std::string endpoint{
            "https://api.deepseek.com/chat/completions"};
        std::string model{"deepseek-v4-flash"};
        std::chrono::milliseconds timeout{10'000};
        std::string api_key;
    };

    [[nodiscard]] DeepSeekClientConfig deepseek_config_from_environment(
        DeepSeekClientConfig config = {});

    class DeepSeekClient {
    public:
        explicit DeepSeekClient(DeepSeekClientConfig config);

        DeepSeekClient(const DeepSeekClient&) = delete;
        DeepSeekClient& operator=(const DeepSeekClient&) = delete;
        DeepSeekClient(DeepSeekClient&&) = delete;
        DeepSeekClient& operator=(DeepSeekClient&&) = delete;

        [[nodiscard]] std::string complete(
            const DeepSeekPrompt& prompt) const;

    private:
        DeepSeekClientConfig config_;
    };
}  // namespace exchange
