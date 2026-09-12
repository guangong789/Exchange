#pragma once

#include "core/types.hpp"

#include <variant>

#include "matching/order.hpp"

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
