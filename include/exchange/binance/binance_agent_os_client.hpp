#pragma once

#include "exchange/core/types.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include "exchange/agent/agent.hpp"
#include "exchange/agent/external_market.hpp"

namespace exchange {
    class BinanceAgentOsError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class BinanceAgentOsTransportError final : public BinanceAgentOsError {
    public:
        using BinanceAgentOsError::BinanceAgentOsError;
    };

    class BinanceAgentOsAuthenticationError final
        : public BinanceAgentOsError {
    public:
        using BinanceAgentOsError::BinanceAgentOsError;
    };

    class BinanceAgentOsProviderError final : public BinanceAgentOsError {
    public:
        using BinanceAgentOsError::BinanceAgentOsError;
    };

    [[nodiscard]] Price parse_scaled_decimal_price(
        std::string_view decimal,
        Price local_price_scale);

    class BinanceAgentOsClient {
    public:
        virtual ~BinanceAgentOsClient() = default;

        [[nodiscard]] virtual ExternalMarketSnapshot fetch_market_snapshot(
            std::string_view symbol) = 0;
    };

    struct BinanceMcpConfig {
        std::string endpoint{"https://agent.binance.com/mcp/agentic"};
        Price local_price_scale{100};
        std::chrono::milliseconds timeout{10'000};
    };

    class BinanceMcpClient final : public BinanceAgentOsClient {
    public:
        explicit BinanceMcpClient(BinanceMcpConfig config = {});

        BinanceMcpClient(const BinanceMcpClient&) = delete;
        BinanceMcpClient& operator=(const BinanceMcpClient&) = delete;
        BinanceMcpClient(BinanceMcpClient&&) = delete;
        BinanceMcpClient& operator=(BinanceMcpClient&&) = delete;

        [[nodiscard]] ExternalMarketSnapshot fetch_market_snapshot(
            std::string_view symbol) override;

    private:
        BinanceMcpConfig config_;
    };

    class BinanceAgentOsObservationBridge {
    public:
        BinanceAgentOsObservationBridge(
            BinanceAgentOsClient& client,
            std::string symbol);

        [[nodiscard]] AgentObservation attach_external_market(
            AgentObservation local_observation);

    private:
        BinanceAgentOsClient& client_;
        std::string symbol_;
    };
}  // namespace exchange
