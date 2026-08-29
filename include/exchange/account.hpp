#pragma once

#include <cstdint>

namespace exchange {
    using AccountId = std::uint64_t;
    using AssetId = std::uint32_t;
    using Amount = std::int64_t;

    struct Balance {
        Amount available{};
        Amount reserved{};

        bool operator==(const Balance&) const = default;
    };
}  // namespace exchange
