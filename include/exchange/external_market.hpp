#pragma once

#include <string>

#include "exchange/types.hpp"

namespace exchange {
    struct ExternalMarketSnapshot {
        std::string symbol;
        Price best_bid{};
        Price best_ask{};

        bool operator==(const ExternalMarketSnapshot&) const = default;
    };
}  // namespace exchange
