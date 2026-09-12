#pragma once

#include <cstdint>
#include <optional>

#include "agent/domain/external_market.hpp"
#include "core/types.hpp"

namespace exchange {
    struct InternalMarketState {
        std::optional<Price> best_bid;
        std::optional<Price> best_ask;

        bool operator==(const InternalMarketState&) const = default;
    };

    struct WorldState {
        std::uint64_t step{};
        InternalMarketState internal_market;
        std::optional<ExternalMarketState> external_market;

        bool operator==(const WorldState&) const = default;
    };
}  // namespace exchange
