#pragma once

#include <cstdint>

namespace exchange {
    using OrderId = std::uint64_t;
    using Price = std::int64_t;
    using Quantity = std::int64_t;
    using Timestamp = std::int64_t;

    enum class Side : std::uint8_t {
        Buy,
        Sell,
    };

    enum class OrderType : std::uint8_t {
        Limit,
    };
}
