#pragma once

#include <chrono>
#include <string>

#include "exchange/model/model_adapter.hpp"

namespace exchange {
    struct DeepSeekConfig {
        std::string endpoint{
            "https://api.deepseek.com/chat/completions"};
        std::string model{"deepseek-v4-flash"};
        std::chrono::milliseconds timeout{10'000};
    };

    class DeepSeekAdapter final : public ModelAdapter {
    public:
        explicit DeepSeekAdapter(DeepSeekConfig config = {});

        DeepSeekAdapter(const DeepSeekAdapter&) = delete;
        DeepSeekAdapter& operator=(const DeepSeekAdapter&) = delete;
        DeepSeekAdapter(DeepSeekAdapter&&) = delete;
        DeepSeekAdapter& operator=(DeepSeekAdapter&&) = delete;

        [[nodiscard]] ModelResponse invoke(
            const ModelRequest& request) override;

    private:
        DeepSeekConfig config_;
        std::string api_key_;
    };
}  // namespace exchange
