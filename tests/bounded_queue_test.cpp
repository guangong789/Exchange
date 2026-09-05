#include "exchange/core/bounded_queue.hpp"

#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        using namespace std::chrono_literals;

        constexpr auto kBlockedCheckTimeout = 25ms;
        constexpr auto kWakeTimeout = 1s;

        TEST(BoundedQueueTest, RejectsZeroCapacity) {
            EXPECT_THROW(BoundedQueue<int>{0}, std::invalid_argument);
        }

        TEST(BoundedQueueTest, PreservesFifoOrdering) {
            BoundedQueue<int> queue{3};

            ASSERT_TRUE(queue.try_push(10));
            ASSERT_TRUE(queue.try_push(20));
            ASSERT_TRUE(queue.try_push(30));

            EXPECT_EQ(queue.try_pop(), std::optional<int>{10});
            EXPECT_EQ(queue.try_pop(), std::optional<int>{20});
            EXPECT_EQ(queue.try_pop(), std::optional<int>{30});
        }

        TEST(BoundedQueueTest, TryPushReturnsFalseWhenFull) {
            BoundedQueue<int> queue{1};

            ASSERT_TRUE(queue.try_push(10));
            EXPECT_FALSE(queue.try_push(20));
            EXPECT_EQ(queue.try_pop(), std::optional<int>{10});
        }

        TEST(BoundedQueueTest, TryPopReturnsNulloptWhenEmpty) {
            BoundedQueue<int> queue{1};

            EXPECT_EQ(queue.try_pop(), std::nullopt);
        }

        TEST(BoundedQueueTest, BlockedProducerWakesAfterPop) {
            BoundedQueue<int> queue{1};
            ASSERT_TRUE(queue.try_push(10));

            std::promise<void> started;
            auto started_future = started.get_future();
            auto producer = std::async(
                std::launch::async,
                [&] {
                    started.set_value();
                    return queue.wait_push(20);
                });

            started_future.wait();
            EXPECT_EQ(producer.wait_for(kBlockedCheckTimeout),
                      std::future_status::timeout);
            EXPECT_EQ(queue.try_pop(), std::optional<int>{10});

            if (producer.wait_for(kWakeTimeout) != std::future_status::ready) {
                queue.close_and_discard();
                FAIL() << "blocked producer did not wake after space became available";
            }
            EXPECT_TRUE(producer.get());
            EXPECT_EQ(queue.try_pop(), std::optional<int>{20});
        }

        TEST(BoundedQueueTest, BlockedConsumerWakesAfterPush) {
            BoundedQueue<int> queue{1};

            std::promise<void> started;
            auto started_future = started.get_future();
            auto consumer = std::async(
                std::launch::async,
                [&] {
                    started.set_value();
                    return queue.wait_pop();
                });

            started_future.wait();
            EXPECT_EQ(consumer.wait_for(kBlockedCheckTimeout),
                      std::future_status::timeout);
            ASSERT_TRUE(queue.try_push(10));

            if (consumer.wait_for(kWakeTimeout) != std::future_status::ready) {
                queue.close_and_discard();
                FAIL() << "blocked consumer did not wake after an item was pushed";
            }
            EXPECT_EQ(consumer.get(), std::optional<int>{10});
        }

        TEST(BoundedQueueTest, CloseWakesBlockedProducer) {
            BoundedQueue<int> queue{1};
            ASSERT_TRUE(queue.try_push(10));

            std::promise<void> started;
            auto started_future = started.get_future();
            auto producer = std::async(
                std::launch::async,
                [&] {
                    started.set_value();
                    return queue.wait_push(20);
                });

            started_future.wait();
            EXPECT_EQ(producer.wait_for(kBlockedCheckTimeout),
                      std::future_status::timeout);
            queue.close_and_discard();

            ASSERT_EQ(producer.wait_for(kWakeTimeout), std::future_status::ready);
            EXPECT_FALSE(producer.get());
        }

        TEST(BoundedQueueTest, CloseWakesBlockedConsumer) {
            BoundedQueue<int> queue{1};

            std::promise<void> started;
            auto started_future = started.get_future();
            auto consumer = std::async(
                std::launch::async,
                [&] {
                    started.set_value();
                    return queue.wait_pop();
                });

            started_future.wait();
            EXPECT_EQ(consumer.wait_for(kBlockedCheckTimeout),
                      std::future_status::timeout);
            queue.close_and_discard();

            ASSERT_EQ(consumer.wait_for(kWakeTimeout), std::future_status::ready);
            EXPECT_EQ(consumer.get(), std::nullopt);
        }

        TEST(BoundedQueueTest, CloseDiscardsQueuedItems) {
            BoundedQueue<int> queue{2};
            ASSERT_TRUE(queue.try_push(10));
            ASSERT_TRUE(queue.try_push(20));

            queue.close_and_discard();

            EXPECT_EQ(queue.try_pop(), std::nullopt);
            EXPECT_EQ(queue.wait_pop(), std::nullopt);
            EXPECT_FALSE(queue.try_push(30));
            EXPECT_FALSE(queue.wait_push(40));
        }

        TEST(BoundedQueueTest, RepeatedCloseIsSafe) {
            BoundedQueue<int> queue{1};

            queue.close_and_discard();
            queue.close_and_discard();

            EXPECT_FALSE(queue.try_push(10));
            EXPECT_FALSE(queue.wait_push(20));
            EXPECT_EQ(queue.try_pop(), std::nullopt);
            EXPECT_EQ(queue.wait_pop(), std::nullopt);
        }
    }  // namespace
}  // namespace exchange
