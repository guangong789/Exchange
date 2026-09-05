#pragma once

#include "exchange/core/types.hpp"

#include <variant>

#include "exchange/matching/order.hpp"
#include "exchange/matching/trade.hpp"

namespace exchange {
    struct OrderAccepted {
        Order order;
    };

    struct OrderCancelled {
        Order order;
    };

    struct TradeCreated {
        Trade trade;

        bool operator==(const TradeCreated&) const = default;
    };

    struct OrderFilled {
        OrderId order_id{};
        Side side{Side::Buy};
        Quantity filled_quantity{};

        bool operator==(const OrderFilled&) const = default;
    };

    struct OrderPartiallyFilled {
        OrderId order_id{};
        Side side{Side::Buy};
        Quantity filled_quantity{};
        Quantity remaining_quantity{};

        bool operator==(const OrderPartiallyFilled&) const = default;
    };

    using EventPayload = std::variant<OrderAccepted,
                                    OrderCancelled,
                                    TradeCreated,
                                    OrderFilled,
                                    OrderPartiallyFilled>;

    struct Event {
        EventPayload payload;
    };
}  // namespace exchange
