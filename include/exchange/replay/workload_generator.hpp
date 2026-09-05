#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "exchange/matching/command.hpp"

namespace exchange {
    struct WorkloadConfig {
        std::size_t command_count{};
        std::uint32_t buy_ratio_bps{5000};
        std::uint32_t cancel_ratio_bps{};
        Price base_price{};
        Price price_variation{};
        Quantity min_quantity{1};
        Quantity max_quantity{1};
        std::uint64_t seed{};
    };

    class SyntheticWorkloadGenerator {
    public:
        [[nodiscard]] std::vector<Command> generate(const WorkloadConfig& config) const;
    };

}
