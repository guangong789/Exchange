#pragma once

#include <cstdint>

#include "agent/domain/external_market.hpp"

namespace exchange {
    class ExternalMarketFeed {
    public:
        virtual ~ExternalMarketFeed() = default;

        [[nodiscard]] virtual ExternalMarketState latest(
            std::int64_t local_now_ms) const = 0;
    };
}  // namespace exchange
