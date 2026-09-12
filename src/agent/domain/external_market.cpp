#include "agent/domain/external_market.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    std::string format_external_price(ExternalPrice price) {
        if (price.units < 0 || price.decimal_places > 18) {
            throw std::invalid_argument("Invalid external price");
        }

        std::string digits = std::to_string(price.units);
        const std::size_t decimal_places = price.decimal_places;
        if (decimal_places == 0) {
            return digits;
        }
        if (digits.size() <= decimal_places) {
            digits.insert(
                0,
                decimal_places + 1 - digits.size(),
                '0');
        }
        digits.insert(digits.size() - decimal_places, 1, '.');
        return digits;
    }
}  // namespace exchange
