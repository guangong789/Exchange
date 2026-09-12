#pragma once

#include <limits>
#include <optional>

#include "accounting/account.hpp"
#include "core/types.hpp"

namespace exchange {
    struct AgentEconomicProfile {
        Quantity max_order_quantity{
            std::numeric_limits<Quantity>::max()};
        Amount max_order_notional{
            std::numeric_limits<Amount>::max()};
        Amount max_base_position{
            std::numeric_limits<Amount>::max()};
        std::optional<Price> max_buy_price;
        std::optional<Price> min_sell_price;

        bool operator==(const AgentEconomicProfile&) const = default;
    };
}  // namespace exchange
