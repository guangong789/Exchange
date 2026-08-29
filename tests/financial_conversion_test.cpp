#include "exchange/financial_conversion.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext simple_instrument{
            10,
            20,
            1'000,
            1,
            1,
        };

        TEST(FinancialConversionTest, CalculatesBaseAndQuoteAtomicAmounts) {
            EXPECT_EQ(
                calculate_trade_amounts(simple_instrument, 100, 3),
                (TradeFinancialAmounts{3'000, 300}));
        }

        TEST(FinancialConversionTest, CalculatesBuyReservationAtLimitPrice) {
            EXPECT_EQ(
                calculate_order_reservation(
                    simple_instrument,
                    Side::Buy,
                    100,
                    3),
                (ReservationRequirement{20, 300}));
        }

        TEST(FinancialConversionTest, CalculatesSellReservationInBaseAsset) {
            EXPECT_EQ(
                calculate_order_reservation(
                    simple_instrument,
                    Side::Sell,
                    100,
                    3),
                (ReservationRequirement{10, 3'000}));
        }

        TEST(FinancialConversionTest, AppliesExactRationalQuoteScale) {
            const InstrumentContext instrument{10, 20, 25, 3, 2};

            EXPECT_EQ(
                calculate_trade_amounts(instrument, 4, 5),
                (TradeFinancialAmounts{125, 30}));
        }

        TEST(FinancialConversionTest, RequiresExactPerQuantityUnitQuoteRate) {
            const InstrumentContext instrument{10, 20, 1, 1, 2};

            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(instrument, 3, 2)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsInvalidAssetConfiguration) {
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{0, 20, 1, 1, 1}, 1, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 0, 1, 1, 1}, 1, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 10, 1, 1, 1}, 1, 1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsNonPositiveBaseScale) {
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, 0, 1, 1}, 1, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, -1, 1, 1}, 1, 1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsNonPositiveQuoteNumerator) {
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, 1, 0, 1}, 1, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, 1, -1, 1}, 1, 1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsNonPositiveQuoteDenominator) {
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, 1, 1, 0}, 1, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_trade_amounts(
                    InstrumentContext{10, 20, 1, 1, -1}, 1, 1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsNonPositivePrice) {
            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(simple_instrument, 0, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(simple_instrument, -1, 1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsNonPositiveQuantity) {
            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(simple_instrument, 1, 0)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(simple_instrument, 1, -1)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsInvalidSide) {
            EXPECT_THROW(
                static_cast<void>(calculate_order_reservation(
                    simple_instrument,
                    static_cast<Side>(99),
                    100,
                    3)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, SellStillRequiresValidFinancialPrice) {
            const InstrumentContext instrument{10, 20, 1, 1, 2};

            EXPECT_THROW(
                static_cast<void>(calculate_order_reservation(
                    instrument,
                    Side::Sell,
                    3,
                    2)),
                std::invalid_argument);
        }

        TEST(FinancialConversionTest, RejectsBaseAmountOverflow) {
            const Amount maximum = std::numeric_limits<Amount>::max();
            const InstrumentContext instrument{10, 20, maximum, 1, 1};

            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(instrument, 1, 2)),
                std::overflow_error);
        }

        TEST(FinancialConversionTest, SupportsWideQuoteIntermediate) {
            const Amount maximum = std::numeric_limits<Amount>::max();
            const InstrumentContext instrument{
                10,
                20,
                1,
                maximum,
                maximum,
            };

            EXPECT_EQ(
                calculate_trade_amounts(instrument, maximum, 1),
                (TradeFinancialAmounts{1, maximum}));
        }

        TEST(FinancialConversionTest, RejectsFinalQuoteAmountOverflow) {
            const Amount maximum = std::numeric_limits<Amount>::max();
            const InstrumentContext instrument{10, 20, 1, 2, 1};

            EXPECT_THROW(
                static_cast<void>(
                    calculate_trade_amounts(instrument, maximum, 1)),
                std::overflow_error);
        }

        TEST(FinancialConversionTest, IdenticalInputsAreDeterministic) {
            const InstrumentContext instrument{10, 20, 25, 3, 2};

            const auto first = calculate_trade_amounts(instrument, 4, 5);
            const auto second = calculate_trade_amounts(instrument, 4, 5);

            EXPECT_EQ(first, second);
        }

        TEST(FinancialConversionTest, PreservesPriceImprovementResidual) {
            const ReservationRequirement reservation =
                calculate_order_reservation(
                    simple_instrument,
                    Side::Buy,
                    100,
                    3);
            const Amount execution_total =
                calculate_trade_amounts(simple_instrument, 90, 1).quote_amount
                + calculate_trade_amounts(
                      simple_instrument, 95, 1).quote_amount
                + calculate_trade_amounts(
                      simple_instrument, 100, 1).quote_amount;

            EXPECT_EQ(reservation.amount, 300);
            EXPECT_EQ(execution_total, 285);
            EXPECT_EQ(reservation.amount - execution_total, 15);
        }

        TEST(FinancialConversionTest, SamePricePartialFillsAccumulateExactly) {
            const auto first =
                calculate_trade_amounts(simple_instrument, 100, 1);
            const auto second =
                calculate_trade_amounts(simple_instrument, 100, 2);
            const auto combined =
                calculate_trade_amounts(simple_instrument, 100, 3);

            EXPECT_EQ(first.base_amount + second.base_amount,
                      combined.base_amount);
            EXPECT_EQ(first.quote_amount + second.quote_amount,
                      combined.quote_amount);
        }

        TEST(FinancialConversionTest, DifferentPricesAreConvertedIndependently) {
            const Amount quote_total =
                calculate_trade_amounts(simple_instrument, 90, 1).quote_amount
                + calculate_trade_amounts(
                      simple_instrument, 95, 2).quote_amount;

            EXPECT_EQ(quote_total, 280);
        }

        TEST(FinancialConversionTest, ExactRateSupportsQuantityOne) {
            const InstrumentContext instrument{10, 20, 7, 1, 3};

            EXPECT_EQ(
                calculate_trade_amounts(instrument, 6, 1),
                (TradeFinancialAmounts{7, 2}));
        }
    }  // namespace
}  // namespace exchange
