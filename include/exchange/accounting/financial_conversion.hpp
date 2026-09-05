#pragma once

#include "exchange/accounting/account.hpp"
#include "exchange/core/types.hpp"

namespace exchange {
    struct InstrumentContext {
        AssetId base_asset{};
        AssetId quote_asset{};
        Amount base_atomic_units_per_quantity_unit{};
        Amount quote_atomic_units_per_price_quantity_numerator{};
        Amount quote_atomic_units_per_price_quantity_denominator{};

        bool operator==(const InstrumentContext&) const = default;
    };

    struct TradeFinancialAmounts {
        Amount base_amount{};
        Amount quote_amount{};

        bool operator==(const TradeFinancialAmounts&) const = default;
    };

    struct ReservationRequirement {
        AssetId asset_id{};
        Amount amount{};

        bool operator==(const ReservationRequirement&) const = default;
    };

    void validate_instrument_context(
        const InstrumentContext& instrument);

    [[nodiscard]] TradeFinancialAmounts calculate_trade_amounts(
        const InstrumentContext& instrument,
        Price price,
        Quantity quantity);

    [[nodiscard]] ReservationRequirement calculate_order_reservation(
        const InstrumentContext& instrument,
        Side side,
        Price limit_price,
        Quantity quantity);
}  // namespace exchange
