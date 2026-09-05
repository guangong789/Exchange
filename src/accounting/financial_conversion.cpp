#include "exchange/accounting/financial_conversion.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    namespace {
#if defined(__SIZEOF_INT128__)
        __extension__ typedef __int128 WideAmount;
#else
#error "financial conversion requires compiler support for signed 128-bit integers"
#endif

        void validate_trading_values(Price price, Quantity quantity) {
            if (price <= 0) {
                throw std::invalid_argument("price must be positive");
            }
            if (quantity <= 0) {
                throw std::invalid_argument("quantity must be positive");
            }
        }

        Amount calculate_base_amount(
            const InstrumentContext& instrument,
            Quantity quantity) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            if (instrument.base_atomic_units_per_quantity_unit
                > maximum / quantity) {
                throw std::overflow_error("base amount overflow");
            }

            return quantity
                * instrument.base_atomic_units_per_quantity_unit;
        }

        Amount calculate_quote_amount(
            const InstrumentContext& instrument,
            Price price,
            Quantity quantity) {
            const WideAmount wide_product =
                static_cast<WideAmount>(price)
                * static_cast<WideAmount>(
                    instrument
                        .quote_atomic_units_per_price_quantity_numerator);
            const WideAmount denominator = static_cast<WideAmount>(
                instrument
                    .quote_atomic_units_per_price_quantity_denominator);

            if (wide_product % denominator != 0) {
                throw std::invalid_argument(
                    "price is not exactly representable in quote atomic units");
            }

            const WideAmount quote_rate = wide_product / denominator;
            const WideAmount maximum = static_cast<WideAmount>(
                std::numeric_limits<Amount>::max());
            const WideAmount wide_quantity =
                static_cast<WideAmount>(quantity);
            if (quote_rate > maximum / wide_quantity) {
                throw std::overflow_error("quote amount overflow");
            }

            return static_cast<Amount>(quote_rate * wide_quantity);
        }

        void validate_side(Side side) {
            if (side != Side::Buy && side != Side::Sell) {
                throw std::invalid_argument("invalid order side");
            }
        }
    }  // namespace

    void validate_instrument_context(
        const InstrumentContext& instrument) {
        if (instrument.base_asset == 0) {
            throw std::invalid_argument("base asset must be non-zero");
        }
        if (instrument.quote_asset == 0) {
            throw std::invalid_argument("quote asset must be non-zero");
        }
        if (instrument.base_asset == instrument.quote_asset) {
            throw std::invalid_argument(
                "base and quote assets must be distinct");
        }
        if (instrument.base_atomic_units_per_quantity_unit <= 0) {
            throw std::invalid_argument(
                "base atomic-unit scale must be positive");
        }
        if (instrument.quote_atomic_units_per_price_quantity_numerator
            <= 0) {
            throw std::invalid_argument(
                "quote scale numerator must be positive");
        }
        if (instrument.quote_atomic_units_per_price_quantity_denominator
            <= 0) {
            throw std::invalid_argument(
                "quote scale denominator must be positive");
        }
    }

    TradeFinancialAmounts calculate_trade_amounts(
        const InstrumentContext& instrument,
        Price price,
        Quantity quantity) {
        validate_instrument_context(instrument);
        validate_trading_values(price, quantity);

        return TradeFinancialAmounts{
            calculate_base_amount(instrument, quantity),
            calculate_quote_amount(instrument, price, quantity),
        };
    }

    ReservationRequirement calculate_order_reservation(
        const InstrumentContext& instrument,
        Side side,
        Price limit_price,
        Quantity quantity) {
        validate_side(side);
        const TradeFinancialAmounts amounts = calculate_trade_amounts(
            instrument,
            limit_price,
            quantity);

        if (side == Side::Buy) {
            return ReservationRequirement{
                instrument.quote_asset,
                amounts.quote_amount,
            };
        }
        return ReservationRequirement{
            instrument.base_asset,
            amounts.base_amount,
        };
    }
}  // namespace exchange
