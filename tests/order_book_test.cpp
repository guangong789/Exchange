#include "exchange/matching/order_book.hpp"

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

        void expect_preview_trade_parity(
            const std::vector<MatchPreview>& previews,
            const std::vector<Trade>& trades,
            Side incoming_side) {
            ASSERT_EQ(previews.size(), trades.size());
            for (std::size_t index = 0; index < previews.size(); ++index) {
                const OrderId maker_order_id =
                    incoming_side == Side::Buy
                        ? trades[index].sell_order_id
                        : trades[index].buy_order_id;
                EXPECT_EQ(previews[index].maker_order_id, maker_order_id);
                EXPECT_EQ(previews[index].execution_price,
                          trades[index].price);
                EXPECT_EQ(previews[index].execution_quantity,
                          trades[index].quantity);
            }
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

        TEST(OrderBookTest, DetailedCancelReturnsCancelledOrder) {
            OrderBook book;
            const Order order = limit_order(1, Side::Buy, 101, 7, 42);
            add_resting(book, order);

            const auto cancelled = book.cancel_order_with_result(1);

            ASSERT_TRUE(cancelled.has_value());
            EXPECT_EQ(cancelled->id, order.id);
            EXPECT_EQ(cancelled->side, order.side);
            EXPECT_EQ(cancelled->type, order.type);
            EXPECT_EQ(cancelled->price, order.price);
            EXPECT_EQ(cancelled->quantity, order.quantity);
            EXPECT_EQ(cancelled->timestamp, order.timestamp);
            EXPECT_EQ(book.order_count(), 0U);
        }

        TEST(OrderBookTest, DetailedCancelReturnsPartiallyFilledRemainder) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 100, 10, 20));
            ASSERT_EQ(book.add_order(limit_order(2, Side::Buy, 100, 4)).size(),
                      1U);

            const auto cancelled = book.cancel_order_with_result(1);

            ASSERT_TRUE(cancelled.has_value());
            EXPECT_EQ(cancelled->id, 1U);
            EXPECT_EQ(cancelled->quantity, 6);
            EXPECT_EQ(cancelled->timestamp, 20);
            EXPECT_EQ(book.order_count(), 0U);
        }

        TEST(OrderBookTest, FailedDetailedCancelReturnsNoOrderAndPreservesState) {
            OrderBook book;
            const Order order = limit_order(1, Side::Buy, 100, 3);
            add_resting(book, order);

            EXPECT_FALSE(book.cancel_order_with_result(999).has_value());
            EXPECT_EQ(book.order_count(), 1U);
            ASSERT_TRUE(book.find_order(1).has_value());
            EXPECT_EQ(book.find_order(1)->quantity, order.quantity);
            EXPECT_EQ(book.best_bid(), order.price);
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

        TEST(OrderBookPreviewTest, EmptyBookHasNoMatches) {
            const OrderBook book;

            EXPECT_TRUE(
                book.preview_matches(
                        limit_order(1, Side::Buy, 100, 5))
                    .empty());
        }

        TEST(OrderBookPreviewTest, NonCrossingBuyHasNoMatches) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 101, 3));

            EXPECT_TRUE(
                book.preview_matches(
                        limit_order(2, Side::Buy, 100, 3))
                    .empty());
        }

        TEST(OrderBookPreviewTest, NonCrossingSellHasNoMatches) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Buy, 99, 3));

            EXPECT_TRUE(
                book.preview_matches(
                        limit_order(2, Side::Sell, 100, 3))
                    .empty());
        }

        TEST(OrderBookPreviewTest, BuyCrossesOneAskAtMakerPrice) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 99, 3));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(2, Side::Buy, 100, 2)),
                (std::vector<MatchPreview>{{1, 99, 2}}));
        }

        TEST(OrderBookPreviewTest, SellCrossesOneBidAtMakerPrice) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Buy, 101, 3));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(2, Side::Sell, 100, 2)),
                (std::vector<MatchPreview>{{1, 101, 2}}));
        }

        TEST(OrderBookPreviewTest, BuyUsesPriceTimePriorityAcrossAskLevels) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 102, 2));
            add_resting(book, limit_order(2, Side::Sell, 100, 1));
            add_resting(book, limit_order(3, Side::Sell, 100, 2));
            add_resting(book, limit_order(4, Side::Sell, 101, 1));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(5, Side::Buy, 102, 5)),
                (std::vector<MatchPreview>{
                    {2, 100, 1},
                    {3, 100, 2},
                    {4, 101, 1},
                    {1, 102, 1},
                }));
        }

        TEST(OrderBookPreviewTest, SellUsesPriceTimePriorityAcrossBidLevels) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Buy, 100, 2));
            add_resting(book, limit_order(2, Side::Buy, 102, 1));
            add_resting(book, limit_order(3, Side::Buy, 102, 2));
            add_resting(book, limit_order(4, Side::Buy, 101, 1));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(5, Side::Sell, 100, 5)),
                (std::vector<MatchPreview>{
                    {2, 102, 1},
                    {3, 102, 2},
                    {4, 101, 1},
                    {1, 100, 1},
                }));
        }

        TEST(OrderBookPreviewTest, PreservesFifoAtTheSamePrice) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 100, 1, 50));
            add_resting(book, limit_order(2, Side::Sell, 100, 1, 10));
            add_resting(book, limit_order(3, Side::Sell, 100, 1, 30));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(4, Side::Buy, 100, 3)),
                (std::vector<MatchPreview>{
                    {1, 100, 1},
                    {2, 100, 1},
                    {3, 100, 1},
                }));
        }

        TEST(OrderBookPreviewTest, PartiallyConsumesFinalMakerWithoutMutation) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 100, 2));
            add_resting(book, limit_order(2, Side::Sell, 101, 5));
            const std::size_t order_count_before = book.order_count();

            const auto first = book.preview_matches(
                limit_order(3, Side::Buy, 101, 4));
            const auto second = book.preview_matches(
                limit_order(3, Side::Buy, 101, 4));

            EXPECT_EQ(first,
                      (std::vector<MatchPreview>{
                          {1, 100, 2},
                          {2, 101, 2},
                      }));
            EXPECT_EQ(second, first);
            EXPECT_EQ(book.order_count(), order_count_before);
            ASSERT_TRUE(book.find_order(1).has_value());
            ASSERT_TRUE(book.find_order(2).has_value());
            EXPECT_EQ(book.find_order(1)->quantity, 2);
            EXPECT_EQ(book.find_order(2)->quantity, 5);
        }

        TEST(OrderBookPreviewTest, StopsWhenIncomingQuantityIsExhausted) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 100, 1));
            add_resting(book, limit_order(2, Side::Sell, 101, 1));
            add_resting(book, limit_order(3, Side::Sell, 102, 1));

            EXPECT_EQ(
                book.preview_matches(
                    limit_order(4, Side::Buy, 102, 2)),
                (std::vector<MatchPreview>{
                    {1, 100, 1},
                    {2, 101, 1},
                }));
        }

        TEST(OrderBookPreviewTest, SharesExecuteOrderValidation) {
            OrderBook preview_book;
            OrderBook execution_book;
            add_resting(preview_book, limit_order(50, Side::Buy, 90, 1));
            add_resting(execution_book, limit_order(50, Side::Buy, 90, 1));
            const std::vector<Order> invalid_orders{
                limit_order(0, Side::Buy, 100, 1),
                limit_order(1, Side::Buy, 0, 1),
                limit_order(2, Side::Buy, -1, 1),
                limit_order(3, Side::Buy, 100, 0),
                limit_order(4, Side::Buy, 100, -1),
                Order{5,
                      Side::Buy,
                      static_cast<OrderType>(99),
                      100,
                      1,
                      0},
                limit_order(50, Side::Sell, 100, 1),
            };

            for (const Order& order : invalid_orders) {
                EXPECT_THROW(
                    static_cast<void>(
                        preview_book.preview_matches(order)),
                    std::invalid_argument);
                EXPECT_THROW(
                    static_cast<void>(execution_book.add_order(order)),
                    std::invalid_argument);
            }

            EXPECT_EQ(preview_book.order_count(), 1U);
            EXPECT_EQ(execution_book.order_count(), 1U);
        }

        TEST(OrderBookPreviewParityTest, BuyPreviewMatchesActualTrades) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Sell, 101, 3));
            add_resting(book, limit_order(2, Side::Sell, 100, 1));
            add_resting(book, limit_order(3, Side::Sell, 100, 2));
            add_resting(book, limit_order(4, Side::Sell, 102, 5));
            const Order incoming = limit_order(5, Side::Buy, 102, 8, 77);

            const auto previews = book.preview_matches(incoming);
            const auto trades = book.add_order(incoming);

            expect_preview_trade_parity(previews, trades, incoming.side);
            ASSERT_EQ(previews.size(), 4U);
            EXPECT_EQ(previews.back(), (MatchPreview{4, 102, 2}));
        }

        TEST(OrderBookPreviewParityTest, SellPreviewMatchesActualTrades) {
            OrderBook book;
            add_resting(book, limit_order(1, Side::Buy, 101, 3));
            add_resting(book, limit_order(2, Side::Buy, 102, 1));
            add_resting(book, limit_order(3, Side::Buy, 102, 2));
            add_resting(book, limit_order(4, Side::Buy, 100, 5));
            const Order incoming = limit_order(5, Side::Sell, 100, 8, 77);

            const auto previews = book.preview_matches(incoming);
            const auto trades = book.add_order(incoming);

            expect_preview_trade_parity(previews, trades, incoming.side);
            ASSERT_EQ(previews.size(), 4U);
            EXPECT_EQ(previews.back(), (MatchPreview{4, 100, 2}));
        }
    }
}
