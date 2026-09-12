#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/types.hpp"

namespace exchange {
    enum class ExternalMarketSource {
        BinanceAlpha,
    };

    enum class ExternalMarketFreshness {
        Unavailable,
        Fresh,
        Stale,
    };

    struct ExternalPrice {
        std::int64_t units{};
        std::uint8_t decimal_places{};

        bool operator==(const ExternalPrice&) const = default;
    };

    struct ExternalMarketState {
        ExternalMarketSource source{ExternalMarketSource::BinanceAlpha};
        std::string symbol;
        std::optional<ExternalPrice> latest_trade_price;
        std::optional<ExternalPrice> best_bid;
        std::optional<ExternalPrice> best_ask;
        std::int64_t event_timestamp_ms{};
        std::int64_t local_receive_timestamp_ms{};
        ExternalMarketFreshness freshness{
            ExternalMarketFreshness::Unavailable};

        bool operator==(const ExternalMarketState&) const = default;
    };

    [[nodiscard]] std::string format_external_price(ExternalPrice price);
}  // namespace exchange
