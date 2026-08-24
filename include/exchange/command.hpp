#pragma once

#include <variant>

#include "exchange/order.hpp"

namespace exchange {
    struct AddOrder {
        Order order;
    };

    struct CancelOrder {
        OrderId order_id{};
    };

    using CommandPayload = std::variant<AddOrder, CancelOrder>;

    struct Command {
        CommandPayload payload;
    };
}
