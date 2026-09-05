#pragma once

#include "exchange/core/types.hpp"

namespace exchange {
    struct Order {
        OrderId id{};
        Side side{Side::Buy};
        OrderType type{OrderType::Limit};
        Price price{};
        Quantity quantity{};
        Timestamp timestamp{};
    };
}
