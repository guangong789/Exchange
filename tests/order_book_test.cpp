#include "exchange/order_book.hpp"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {

    Order limit_order(OrderId id, Side side, Price price, Quantity quantity, Timestamp timestamp = 0) {
        return Order{id, side, OrderType::Limit, price, quantity, timestamp};
    }

    void add_resting(OrderBook& book, Order order) {
        EXPECT_TRUE(book.add_order(order).empty());
    }

    TEST(OrderBookTest, StartsEmpty) {
        const OrderBook book;

        EXPECT_EQ(book.order_count(), 0U);
        EXPECT_EQ(book.best_bid(), std::nullopt);
        EXPECT_EQ(book.best_ask(), std::nullopt);
    }

    TEST(OrderBookTest, NonCrossingOrdersRestAtBestPrices) {
        OrderBook book;

        EXPECT_TRUE(book.add_order(limit_order(1, Side::Buy, 100, 5)).empty());
        EXPECT_TRUE(book.add_order(limit_order(2, Side::Buy, 101, 4)).empty());
        EXPECT_TRUE(book.add_order(limit_order(3, Side::Sell, 104, 2)).empty());
        EXPECT_TRUE(book.add_order(limit_order(4, Side::Sell, 103, 3)).empty());

        EXPECT_EQ(book.best_bid(), 101);
        EXPECT_EQ(book.best_ask(), 103);
        EXPECT_EQ(book.order_count(), 4U);
    }

    TEST(OrderBookTest, BuyOrderMatchesLowestAskAtRestingPrice) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 7, 10));

        const auto trades = book.add_order(limit_order(2, Side::Buy, 105, 7, 20));

        ASSERT_EQ(trades.size(), 1U);
        EXPECT_EQ(trades[0], (Trade{2, 1, 100, 7, 20}));
        EXPECT_EQ(book.order_count(), 0U);
    }

    TEST(OrderBookTest, SellOrderMatchesHighestBidAtRestingPrice) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Buy, 105, 6, 10));

        const auto trades = book.add_order(limit_order(2, Side::Sell, 100, 6, 20));

        ASSERT_EQ(trades.size(), 1U);
        EXPECT_EQ(trades[0], (Trade{1, 2, 105, 6, 20}));
        EXPECT_EQ(book.order_count(), 0U);
    }

    TEST(OrderBookTest, IncomingOrderCanBePartiallyFilledAndRest) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 3));

        const auto trades = book.add_order(limit_order(2, Side::Buy, 100, 8));

        ASSERT_EQ(trades.size(), 1U);
        EXPECT_EQ(trades[0].quantity, 3);
        const auto remainder = book.find_order(2);
        ASSERT_TRUE(remainder.has_value());
        EXPECT_EQ(remainder->quantity, 5);
        EXPECT_EQ(book.best_bid(), 100);
        EXPECT_EQ(book.best_ask(), std::nullopt);
    }

    TEST(OrderBookTest, RestingOrderCanBePartiallyFilledAndKeepPriority) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 10));
        EXPECT_EQ(book.add_order(limit_order(2, Side::Buy, 100, 4)).size(), 1U);

        const auto remainder = book.find_order(1);
        ASSERT_TRUE(remainder.has_value());
        EXPECT_EQ(remainder->quantity, 6);

        const auto trades = book.add_order(limit_order(3, Side::Buy, 100, 6));
        ASSERT_EQ(trades.size(), 1U);
        EXPECT_EQ(trades[0].sell_order_id, 1U);
        EXPECT_EQ(trades[0].quantity, 6);
        EXPECT_EQ(book.order_count(), 0U);
    }

    TEST(OrderBookTest, BetterPriceMatchesBeforeWorsePrice) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 102, 2));
        add_resting(book, limit_order(2, Side::Sell, 100, 2));
        add_resting(book, limit_order(3, Side::Sell, 101, 2));

        const auto trades = book.add_order(limit_order(4, Side::Buy, 102, 6));

        ASSERT_EQ(trades.size(), 3U);
        EXPECT_EQ(trades[0].sell_order_id, 2U);
        EXPECT_EQ(trades[0].price, 100);
        EXPECT_EQ(trades[1].sell_order_id, 3U);
        EXPECT_EQ(trades[1].price, 101);
        EXPECT_EQ(trades[2].sell_order_id, 1U);
        EXPECT_EQ(trades[2].price, 102);
    }

    TEST(OrderBookTest, EarlierArrivalMatchesFirstAtSamePrice) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 2, 50));
        add_resting(book, limit_order(2, Side::Sell, 100, 2, 10));

        const auto trades = book.add_order(limit_order(3, Side::Buy, 100, 4));

        ASSERT_EQ(trades.size(), 2U);
        EXPECT_EQ(trades[0].sell_order_id, 1U);
        EXPECT_EQ(trades[1].sell_order_id, 2U);
    }

    TEST(OrderBookTest, LimitPricePreventsMatchingWorseLevels) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 2));
        add_resting(book, limit_order(2, Side::Sell, 101, 2));

        const auto trades = book.add_order(limit_order(3, Side::Buy, 100, 4));

        ASSERT_EQ(trades.size(), 1U);
        EXPECT_EQ(trades[0].sell_order_id, 1U);
        EXPECT_EQ(book.best_bid(), 100);
        EXPECT_EQ(book.best_ask(), 101);
        EXPECT_EQ(book.find_order(3)->quantity, 2);
    }

    TEST(OrderBookTest, CancelRemovesRestingOrderAndEmptyPriceLevel) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Buy, 101, 2));
        add_resting(book, limit_order(2, Side::Buy, 100, 3));

        EXPECT_TRUE(book.cancel_order(1));
        EXPECT_EQ(book.best_bid(), 100);
        EXPECT_EQ(book.order_count(), 1U);
        EXPECT_FALSE(book.find_order(1).has_value());
        EXPECT_FALSE(book.cancel_order(1));
        EXPECT_FALSE(book.cancel_order(999));
    }

    TEST(OrderBookTest, CancelMiddleOrderPreservesFifoForRemainingOrders) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Sell, 100, 1));
        add_resting(book, limit_order(2, Side::Sell, 100, 1));
        add_resting(book, limit_order(3, Side::Sell, 100, 1));
        ASSERT_TRUE(book.cancel_order(2));

        const auto trades = book.add_order(limit_order(4, Side::Buy, 100, 2));

        ASSERT_EQ(trades.size(), 2U);
        EXPECT_EQ(trades[0].sell_order_id, 1U);
        EXPECT_EQ(trades[1].sell_order_id, 3U);
    }

    TEST(OrderBookTest, RejectsInvalidOrdersWithoutChangingBook) {
        OrderBook book;

        EXPECT_THROW(book.add_order(limit_order(0, Side::Buy, 100, 1)),
                    std::invalid_argument);
        EXPECT_THROW(book.add_order(limit_order(1, Side::Buy, 0, 1)),
                    std::invalid_argument);
        EXPECT_THROW(book.add_order(limit_order(2, Side::Buy, -1, 1)),
                    std::invalid_argument);
        EXPECT_THROW(book.add_order(limit_order(3, Side::Buy, 100, 0)),
                    std::invalid_argument);
        EXPECT_THROW(book.add_order(limit_order(4, Side::Buy, 100, -1)),
                    std::invalid_argument);
        EXPECT_EQ(book.order_count(), 0U);
    }

    TEST(OrderBookTest, RejectsDuplicateRestingOrderId) {
        OrderBook book;
        add_resting(book, limit_order(1, Side::Buy, 100, 1));

        EXPECT_THROW(book.add_order(limit_order(1, Side::Sell, 101, 1)),
                    std::invalid_argument);
        EXPECT_EQ(book.order_count(), 1U);
    }

    }  // namespace
}  // namespace exchange