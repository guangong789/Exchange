#include "exchange/replay_engine.hpp"

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

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

        Command add(Order order) {
            return Command{CommandPayload{AddOrder{order}}};
        }

        Command cancel(OrderId order_id) {
            return Command{CommandPayload{CancelOrder{order_id}}};
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

        void expect_event_eq(const Event& actual, const Event& expected) {
            ASSERT_EQ(actual.payload.index(), expected.payload.index());

            std::visit(
                [&expected](const auto& actual_payload) {
                    using Payload = std::decay_t<decltype(actual_payload)>;
                    const auto& expected_payload = std::get<Payload>(expected.payload);

                    if constexpr (std::is_same_v<Payload, OrderAccepted> ||
                                std::is_same_v<Payload, OrderCancelled>) {
                        expect_order_eq(actual_payload.order, expected_payload.order);
                    } else if constexpr (std::is_same_v<Payload, TradeCreated>) {
                        EXPECT_EQ(actual_payload.trade, expected_payload.trade);
                    } else {
                        EXPECT_EQ(actual_payload, expected_payload);
                    }
                },
                actual.payload);
        }

        TEST(ReplayEngineTest, AddOnlyReplayPreservesCommandOrderAndFinalBook) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const std::vector<Command> commands{
                add(limit_order(1, Side::Buy, 100, 5, 10)),
                add(limit_order(2, Side::Buy, 101, 3, 20)),
                add(limit_order(3, Side::Sell, 105, 4, 30)),
            };

            replay_engine.replay(commands);

            ASSERT_EQ(collector.size(), 3U);
            expect_order_eq(payload_at<OrderAccepted>(collector, 0).order,
                            limit_order(1, Side::Buy, 100, 5, 10));
            expect_order_eq(payload_at<OrderAccepted>(collector, 1).order,
                            limit_order(2, Side::Buy, 101, 3, 20));
            expect_order_eq(payload_at<OrderAccepted>(collector, 2).order,
                            limit_order(3, Side::Sell, 105, 4, 30));
            EXPECT_EQ(matching_engine.order_book().best_bid(), 101);
            EXPECT_EQ(matching_engine.order_book().best_ask(), 105);
            EXPECT_EQ(matching_engine.order_book().order_count(), 3U);
        }

        TEST(ReplayEngineTest, AddAndCancelReplayEmitsCancellationAndUpdatesBook) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const Order cancelled_order = limit_order(1, Side::Buy, 100, 5, 10);
            const std::vector<Command> commands{
                add(cancelled_order),
                add(limit_order(2, Side::Sell, 105, 4, 20)),
                cancel(1),
            };

            replay_engine.replay(commands);

            ASSERT_EQ(collector.size(), 3U);
            expect_order_eq(payload_at<OrderCancelled>(collector, 2).order,
                            cancelled_order);
            EXPECT_FALSE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_TRUE(matching_engine.order_book().find_order(2).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
        }

        TEST(ReplayEngineTest, MultiLevelMatchingFollowsPricePriority) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const std::vector<Command> commands{
                add(limit_order(1, Side::Sell, 102, 2)),
                add(limit_order(2, Side::Sell, 100, 2)),
                add(limit_order(3, Side::Sell, 101, 2)),
                add(limit_order(4, Side::Buy, 102, 6)),
            };

            replay_engine.replay(commands);

            ASSERT_EQ(collector.size(), 13U);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 4).trade.sell_order_id, 2U);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 7).trade.sell_order_id, 3U);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 10).trade.sell_order_id, 1U);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 4).trade.price, 100);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 7).trade.price, 101);
            EXPECT_EQ(payload_at<TradeCreated>(collector, 10).trade.price, 102);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
        }

        TEST(ReplayEngineTest, PartialFillReplayLeavesDeterministicRemainder) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const std::vector<Command> commands{
                add(limit_order(1, Side::Sell, 100, 10)),
                add(limit_order(2, Side::Buy, 100, 4)),
            };

            replay_engine.replay(commands);

            ASSERT_EQ(collector.size(), 5U);
            EXPECT_EQ(payload_at<OrderPartiallyFilled>(collector, 3),
                    (OrderPartiallyFilled{1, Side::Sell, 4, 6}));
            EXPECT_EQ(payload_at<OrderFilled>(collector, 4),
                    (OrderFilled{2, Side::Buy, 4}));
            const auto remainder = matching_engine.order_book().find_order(1);
            ASSERT_TRUE(remainder.has_value());
            EXPECT_EQ(remainder->quantity, 6);
        }

        TEST(ReplayEngineTest, SameCommandsProduceSameEventsAndFinalState) {
            const std::vector<Command> commands{
                add(limit_order(1, Side::Sell, 100, 2, 10)),
                add(limit_order(2, Side::Sell, 101, 5, 20)),
                add(limit_order(3, Side::Buy, 101, 4, 30)),
                cancel(2),
                add(limit_order(4, Side::Buy, 99, 3, 40)),
            };

            EventCollector first_collector;
            MatchingEngine first_matching_engine(first_collector);
            ReplayEngine first_replay_engine(first_matching_engine);
            first_replay_engine.replay(commands);

            EventCollector second_collector;
            MatchingEngine second_matching_engine(second_collector);
            ReplayEngine second_replay_engine(second_matching_engine);
            second_replay_engine.replay(commands);

            ASSERT_EQ(first_collector.size(), second_collector.size());
            for (std::size_t index = 0; index < first_collector.size(); ++index) {
                expect_event_eq(first_collector.events()[index],
                                second_collector.events()[index]);
            }

            const OrderBook& first_book = first_matching_engine.order_book();
            const OrderBook& second_book = second_matching_engine.order_book();
            EXPECT_EQ(first_book.best_bid(), second_book.best_bid());
            EXPECT_EQ(first_book.best_ask(), second_book.best_ask());
            EXPECT_EQ(first_book.order_count(), second_book.order_count());

            for (OrderId id = 1; id <= 4; ++id) {
                const auto first_order = first_book.find_order(id);
                const auto second_order = second_book.find_order(id);
                ASSERT_EQ(first_order.has_value(), second_order.has_value());
                if (first_order.has_value()) {
                    expect_order_eq(*first_order, *second_order);
                }
            }
        }

        TEST(ReplayEngineTest, FailedCancellationEmitsNothingAndReplayContinues) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const std::vector<Command> commands{
                cancel(999),
                add(limit_order(1, Side::Buy, 100, 5)),
            };

            replay_engine.replay(commands);

            ASSERT_EQ(collector.size(), 1U);
            EXPECT_TRUE(
                std::holds_alternative<OrderAccepted>(collector.events()[0].payload));
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
        }

        TEST(ReplayEngineTest, InvalidOrderPropagatesAndStopsReplay) {
            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);
            const std::vector<Command> commands{
                add(limit_order(1, Side::Buy, 100, 5)),
                add(limit_order(2, Side::Sell, 0, 3)),
                add(limit_order(3, Side::Sell, 105, 4)),
            };

            EXPECT_THROW(replay_engine.replay(commands), std::invalid_argument);

            ASSERT_EQ(collector.size(), 1U);
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(2).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(3).has_value());
        }
    }
}
