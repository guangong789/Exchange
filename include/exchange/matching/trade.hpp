#pragma once

#include "exchange/core/types.hpp"

namespace exchange {
    struct Trade {
        OrderId buy_order_id{};
        OrderId sell_order_id{};
        Price price{};
        Quantity quantity{};
        Timestamp timestamp{};

        bool operator==(const Trade&) const = default;
    };
}
