#include "exchange/workload_generator.hpp"

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <variant>

#include <gtest/gtest.h>

#include "exchange/event_collector.hpp"
#include "exchange/matching_engine.hpp"
#include "exchange/replay_engine.hpp"

namespace exchange {
    namespace {
        void expect_order_eq(const Order& actual, const Order& expected) {
            EXPECT_EQ(actual.id, expected.id);
            EXPECT_EQ(actual.side, expected.side);
            EXPECT_EQ(actual.type, expected.type);
            EXPECT_EQ(actual.price, expected.price);
            EXPECT_EQ(actual.quantity, expected.quantity);
            EXPECT_EQ(actual.timestamp, expected.timestamp);
        }

        void expect_command_eq(const Command& actual, const Command& expected) {
            ASSERT_EQ(actual.payload.index(), expected.payload.index());
            std::visit(
                [&expected](const auto& actual_payload) {
                    using Payload = std::decay_t<decltype(actual_payload)>;
                    const auto& expected_payload =
                        std::get<Payload>(expected.payload);

                    if constexpr (std::is_same_v<Payload, AddOrder>) {
                        expect_order_eq(actual_payload.order, expected_payload.order);
                    } else {
                        EXPECT_EQ(actual_payload.order_id, expected_payload.order_id);
                    }
                },
                actual.payload);
        }

        WorkloadConfig default_config() {
            return WorkloadConfig{
                .command_count = 100,
                .buy_ratio_bps = 5000,
                .cancel_ratio_bps = 2000,
                .base_price = 10'000,
                .price_variation = 100,
                .min_quantity = 1,
                .max_quantity = 25,
                .seed = 42,
            };
        }

        TEST(SyntheticWorkloadGeneratorTest, ProducesConfiguredCommandCount) {
            const SyntheticWorkloadGenerator generator;
            const auto commands = generator.generate(default_config());

            EXPECT_EQ(commands.size(), 100U);
        }

        TEST(SyntheticWorkloadGeneratorTest, SameConfigAndSeedProduceSameCommands) {
            const SyntheticWorkloadGenerator generator;
            const WorkloadConfig config = default_config();

            const auto first = generator.generate(config);
            const auto second = generator.generate(config);

            ASSERT_EQ(first.size(), second.size());
            for (std::size_t index = 0; index < first.size(); ++index) {
                expect_command_eq(first[index], second[index]);
            }
        }

        TEST(SyntheticWorkloadGeneratorTest, GeneratedOrdersRespectConfiguredBounds) {
            const SyntheticWorkloadGenerator generator;
            WorkloadConfig config = default_config();
            config.command_count = 500;
            config.base_price = 1'000;
            config.price_variation = 25;
            config.min_quantity = 3;
            config.max_quantity = 9;

            const auto commands = generator.generate(config);

            std::unordered_set<OrderId> order_ids;
            OrderId expected_next_id = 1;
            for (std::size_t index = 0; index < commands.size(); ++index) {
                if (const auto* add_order =
                        std::get_if<AddOrder>(&commands[index].payload)) {
                    const Order& order = add_order->order;
                    EXPECT_EQ(order.id, expected_next_id++);
                    EXPECT_TRUE(order_ids.insert(order.id).second);
                    EXPECT_GE(order.price, 975);
                    EXPECT_LE(order.price, 1'025);
                    EXPECT_GE(order.quantity, 3);
                    EXPECT_LE(order.quantity, 9);
                    EXPECT_EQ(order.timestamp, static_cast<Timestamp>(index + 1));
                    EXPECT_EQ(order.type, OrderType::Limit);
                }
            }
        }

        TEST(SyntheticWorkloadGeneratorTest, ExtremeBuySellRatiosAreHonored) {
            const SyntheticWorkloadGenerator generator;
            WorkloadConfig config = default_config();
            config.command_count = 50;
            config.cancel_ratio_bps = 0;
            config.buy_ratio_bps = 10'000;

            const auto buys = generator.generate(config);
            for (const Command& command : buys) {
                EXPECT_EQ(std::get<AddOrder>(command.payload).order.side, Side::Buy);
            }

            config.buy_ratio_bps = 0;
            const auto sells = generator.generate(config);
            for (const Command& command : sells) {
                EXPECT_EQ(std::get<AddOrder>(command.payload).order.side, Side::Sell);
            }
        }

        TEST(SyntheticWorkloadGeneratorTest, CancellationAlwaysTargetsActiveOrder) {
            const SyntheticWorkloadGenerator generator;
            WorkloadConfig config = default_config();
            config.command_count = 300;
            config.cancel_ratio_bps = 7000;
            const auto commands = generator.generate(config);

            EventCollector collector;
            MatchingEngine matching_engine(collector);
            std::size_t cancellation_count = 0;

            for (const Command& command : commands) {
                if (const auto* add_order = std::get_if<AddOrder>(&command.payload)) {
                    static_cast<void>(matching_engine.add_order(add_order->order));
                } else {
                    const OrderId order_id = std::get<CancelOrder>(command.payload).order_id;
                    EXPECT_TRUE(matching_engine.order_book().find_order(order_id).has_value());
                    EXPECT_TRUE(matching_engine.cancel_order(order_id));
                    ++cancellation_count;
                }
            }

            EXPECT_GT(cancellation_count, 0U);
        }

        TEST(SyntheticWorkloadGeneratorTest, EmptyActiveSetFallsBackToAdd) {
            const SyntheticWorkloadGenerator generator;
            WorkloadConfig config = default_config();
            config.command_count = 2;
            config.cancel_ratio_bps = 10'000;
            config.buy_ratio_bps = 10'000;

            const auto commands = generator.generate(config);

            ASSERT_EQ(commands.size(), 2U);
            EXPECT_TRUE(std::holds_alternative<AddOrder>(commands[0].payload));
            EXPECT_TRUE(std::holds_alternative<CancelOrder>(commands[1].payload));
        }

        TEST(SyntheticWorkloadGeneratorTest, GeneratedWorkloadReplaysWithoutError) {
            const SyntheticWorkloadGenerator generator;
            WorkloadConfig config = default_config();
            config.command_count = 1'000;
            const auto commands = generator.generate(config);

            EventCollector collector;
            MatchingEngine matching_engine(collector);
            ReplayEngine replay_engine(matching_engine);

            EXPECT_NO_THROW(replay_engine.replay(commands));
        }

        TEST(SyntheticWorkloadGeneratorTest, RejectsInvalidConfiguration) {
            const SyntheticWorkloadGenerator generator;

            auto config = default_config();
            config.buy_ratio_bps = 10'001;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);

            config = default_config();
            config.cancel_ratio_bps = 10'001;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);

            config = default_config();
            config.base_price = 0;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);

            config = default_config();
            config.price_variation = config.base_price;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);

            config = default_config();
            config.min_quantity = 0;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);

            config = default_config();
            config.min_quantity = 10;
            config.max_quantity = 9;
            EXPECT_THROW(generator.generate(config), std::invalid_argument);
        }
    }
} 
