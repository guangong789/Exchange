#include "exchange/matching_engine.hpp"

#include <stdexcept>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        Order limit_order(OrderId id,
                        Side side,
                        Price price,
                        Quantity quantity,
                        Timestamp timestamp = 0) {
            return Order{id, side, OrderType::Limit, price, quantity, timestamp};
        }

        template <typename Payload>
        const Payload& payload_at(const EventCollector& collector, std::size_t index) {
            return std::get<Payload>(collector.events().at(index).payload);
        }

        void expect_order_eq(const Order& actual, const Order& expected) {
            EXPECT_EQ(actual.id, expected.id);
            EXPECT_EQ(actual.side, expected.side);
            EXPECT_EQ(actual.type, expected.type);
            EXPECT_EQ(actual.price, expected.price);
            EXPECT_EQ(actual.quantity, expected.quantity);
            EXPECT_EQ(actual.timestamp, expected.timestamp);
        }

        TEST(EventCollectorTest, StoresEventsInPublicationOrderAndCanClear) {
            EventCollector collector;
            const Order order = limit_order(1, Side::Buy, 100, 5, 10);
            constexpr std::size_t reserved_capacity = 8;

            collector.reserve(reserved_capacity);
            EXPECT_GE(collector.events().capacity(), reserved_capacity);

            collector.publish(Event{EventPayload{OrderAccepted{order}}});
            collector.publish(Event{EventPayload{OrderCancelled{order}}});

            ASSERT_EQ(collector.size(), 2U);
            expect_order_eq(payload_at<OrderAccepted>(collector, 0).order, order);
            expect_order_eq(payload_at<OrderCancelled>(collector, 1).order, order);

            collector.clear();
            EXPECT_TRUE(collector.empty());
        }

        TEST(MatchingEngineEventTest, AcceptedNonCrossingOrderEmitsOnlyAccepted) {
            EventCollector collector;
            MatchingEngine engine(collector);
            const Order order = limit_order(1, Side::Buy, 100, 5, 10);

            const auto trades = engine.add_order(order);

            EXPECT_TRUE(trades.empty());
            ASSERT_EQ(collector.size(), 1U);
            expect_order_eq(payload_at<OrderAccepted>(collector, 0).order, order);
            ASSERT_TRUE(engine.order_book().find_order(1).has_value());
            expect_order_eq(*engine.order_book().find_order(1), order);
        }

        TEST(MatchingEngineEventTest, InvalidOrderEmitsNoEvent) {
            EventCollector collector;
            MatchingEngine engine(collector);

            EXPECT_THROW(engine.add_order(limit_order(1, Side::Buy, 0, 5)),
                        std::invalid_argument);
            EXPECT_TRUE(collector.empty());
            EXPECT_EQ(engine.order_book().order_count(), 0U);
        }

        TEST(MatchingEngineEventTest, FullMatchEmitsAcceptedTradeAndMakerThenTakerFilled) {
            EventCollector collector;
            MatchingEngine engine(collector);
            const Order maker = limit_order(1, Side::Sell, 100, 5, 10);
            const Order taker = limit_order(2, Side::Buy, 105, 5, 20);
            ASSERT_TRUE(engine.add_order(maker).empty());
            collector.clear();

            const auto trades = engine.add_order(taker);

            ASSERT_EQ(trades.size(), 1U);
            ASSERT_EQ(collector.size(), 4U);
            expect_order_eq(payload_at<OrderAccepted>(collector, 0).order, taker);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 1).trade, trades[0]);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Sell, 5}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 3),
                    (OrderFilled{2, Side::Buy, 5}));
        }

        TEST(MatchingEngineEventTest, SellTakerReportsBuyMakerBeforeSellTaker) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Buy, 105, 5)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(2, Side::Sell, 100, 5));

            ASSERT_EQ(trades.size(), 1U);
            ASSERT_EQ(collector.size(), 4U);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 1).trade, trades[0]);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Buy, 5}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 3),
                    (OrderFilled{2, Side::Sell, 5}));
        }

        TEST(MatchingEngineEventTest, PartialMatchReportsPerTradeQuantityAndRemainingQuantity) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(
                engine.add_order(limit_order(1, Side::Sell, 100, 10)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(2, Side::Buy, 100, 4));

            ASSERT_EQ(trades.size(), 1U);
            ASSERT_EQ(collector.size(), 4U);
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 2),
                    (OrderPartiallyFilled{1, Side::Sell, 4, 6}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 3),
                    (OrderFilled{2, Side::Buy, 4}));
        }

        TEST(MatchingEngineEventTest, MultiFillEventsFollowTradeMakerTakerSequence) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Sell, 100, 2)).empty());
            ASSERT_TRUE(engine.add_order(limit_order(2, Side::Sell, 101, 3)).empty());
            ASSERT_TRUE(engine.add_order(limit_order(3, Side::Sell, 102, 4)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(4, Side::Buy, 102, 9));

            ASSERT_EQ(trades.size(), 3U);
            ASSERT_EQ(collector.size(), 10U);
            EXPECT_TRUE(std::holds_alternative<OrderAccepted>(collector.events()[0].payload));

            EXPECT_EQ(payload_at<TradeCreated>(collector, 1).trade, trades[0]);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Sell, 2}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 3),
                    (OrderPartiallyFilled{4, Side::Buy, 2, 7}));

            EXPECT_EQ(payload_at<TradeCreated>(collector, 4).trade, trades[1]);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 5),
                    (OrderFilled{2, Side::Sell, 3}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 6),
                    (OrderPartiallyFilled{4, Side::Buy, 3, 4}));

            EXPECT_EQ(payload_at<TradeCreated>(collector, 7).trade, trades[2]);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 8),
                    (OrderFilled{3, Side::Sell, 4}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 9),
                    (OrderFilled{4, Side::Buy, 4}));
        }

        TEST(MatchingEngineEventTest, MultiFillFinalMakerCanRemainPartiallyFilled) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Sell, 100, 2)).empty());
            ASSERT_TRUE(engine.add_order(limit_order(2, Side::Sell, 101, 10)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(3, Side::Buy, 101, 5));

            ASSERT_EQ(trades.size(), 2U);
            ASSERT_EQ(collector.size(), 7U);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Sell, 2}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 3),
                    (OrderPartiallyFilled{3, Side::Buy, 2, 3}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 5),
                    (OrderPartiallyFilled{2, Side::Sell, 3, 7}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 6),
                    (OrderFilled{3, Side::Buy, 3}));
        }

        TEST(MatchingEngineEventTest, IncomingRemainderProducesPartialEventAndRests) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Sell, 100, 3)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(2, Side::Buy, 100, 8));

            ASSERT_EQ(trades.size(), 1U);
            ASSERT_EQ(collector.size(), 4U);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Sell, 3}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 3),
                    (OrderPartiallyFilled{2, Side::Buy, 3, 5}));
            ASSERT_TRUE(engine.order_book().find_order(2).has_value());
            EXPECT_EQ(engine.order_book().find_order(2)->quantity, 5);
        }

        TEST(MatchingEngineEventTest, TakerCanRemainAfterConsumingMultipleMakers) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Sell, 100, 2)).empty());
            ASSERT_TRUE(engine.add_order(limit_order(2, Side::Sell, 101, 3)).empty());
            collector.clear();

            const auto trades =
                engine.add_order(limit_order(3, Side::Buy, 101, 8));

            ASSERT_EQ(trades.size(), 2U);
            ASSERT_EQ(collector.size(), 7U);
            EXPECT_EQ(payload_at<OrderFilled>(collector, 2),
                    (OrderFilled{1, Side::Sell, 2}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 3),
                    (OrderPartiallyFilled{3, Side::Buy, 2, 6}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 5),
                    (OrderFilled{2, Side::Sell, 3}));
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 6),
                    (OrderPartiallyFilled{3, Side::Buy, 3, 3}));
            ASSERT_TRUE(engine.order_book().find_order(3).has_value());
            EXPECT_EQ(engine.order_book().find_order(3)->quantity, 3);
        }

        TEST(MatchingEngineEventTest, SuccessfulCancelEmitsRemainingOrderSnapshot) {
            EventCollector collector;
            MatchingEngine engine(collector);
            const Order order = limit_order(1, Side::Buy, 100, 7, 10);
            ASSERT_TRUE(engine.add_order(order).empty());
            collector.clear();

            EXPECT_TRUE(engine.cancel_order(1));

            ASSERT_EQ(collector.size(), 1U);
            expect_order_eq(payload_at<OrderCancelled>(collector, 0).order, order);
            EXPECT_FALSE(engine.order_book().find_order(1).has_value());
        }

        TEST(MatchingEngineEventTest, CancelAfterPartialFillReportsCurrentRemainder) {
            EventCollector collector;
            MatchingEngine engine(collector);
            ASSERT_TRUE(engine.add_order(limit_order(1, Side::Sell, 100, 10)).empty());
            ASSERT_EQ(engine.add_order(limit_order(2, Side::Buy, 100, 4)).size(), 1U);
            collector.clear();

            ASSERT_TRUE(engine.cancel_order(1));

            ASSERT_EQ(collector.size(), 1U);
            const Order& cancelled = payload_at<OrderCancelled>(collector, 0).order;
            EXPECT_EQ(cancelled.id, 1U);
            EXPECT_EQ(cancelled.quantity, 6);
        }

        TEST(MatchingEngineEventTest, FailedCancelEmitsNoEvent) {
            EventCollector collector;
            MatchingEngine engine(collector);

            EXPECT_FALSE(engine.cancel_order(999));
            EXPECT_TRUE(collector.empty());
        }
    }
}
