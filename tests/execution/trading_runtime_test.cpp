#include "execution/trading_runtime.hpp"

#include <stdexcept>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext test_instrument{
            20,
            10,
            1,
            1,
            1,
        };

        TradingRequest submit_request(
            RequestId request_id,
            AccountId account_id,
            Side side,
            Price price,
            Quantity quantity) {
            return TradingRequest{
                request_id,
                account_id,
                SubmitTradingRequest{side, price, quantity}};
        }

        static_assert(!std::is_copy_constructible_v<TradingRuntime>);
        static_assert(!std::is_move_constructible_v<TradingRuntime>);

        TEST(TradingRuntimeTest, ConstructsAndUsesExistingAccountBootstrap) {
            TradingRuntime runtime{test_instrument};

            EXPECT_EQ(runtime.instrument(), test_instrument);
            EXPECT_TRUE(runtime.accounts().create_account(1));
            runtime.accounts().fund(1, 10, 1'000);

            EXPECT_EQ(
                runtime.accounts().find_balance(1, 10),
                (Balance{1'000, 0}));
            EXPECT_EQ(runtime.order_book().order_count(), 0U);
            EXPECT_TRUE(runtime.ledger().entries().empty());
        }

        TEST(TradingRuntimeTest, RejectsInvalidInstrumentAtConstruction) {
            EXPECT_THROW(
                static_cast<void>(TradingRuntime{
                    InstrumentContext{20, 20, 1, 1, 1}}),
                std::invalid_argument);
        }

        TEST(TradingRuntimeTest, ExecutorSubmitsFundedRestingOrder) {
            TradingRuntime runtime{test_instrument};
            ASSERT_TRUE(runtime.accounts().create_account(1));
            runtime.accounts().fund(1, 10, 1'000);

            const TradingResponse response = runtime.executor().execute(
                submit_request(
                    101,
                    1,
                    Side::Buy,
                    100,
                    3));

            EXPECT_EQ(response.request_id, 101U);
            EXPECT_EQ(response.result, TradingResult::Accepted);
            EXPECT_EQ(response.assigned_order_id, 1U);
            EXPECT_EQ(runtime.accounts().find_balance(1, 10),
                      (Balance{700, 300}));
            EXPECT_EQ(runtime.reservations().find(1),
                      (OrderReservation{1, 10, 300, 300}));
            EXPECT_TRUE(runtime.order_book().find_order(1).has_value());
            EXPECT_EQ(runtime.ledger().entries().size(), 1U);
        }

        TEST(TradingRuntimeTest, TwoAccountsTradeWithinOneOwnedState) {
            TradingRuntime runtime{test_instrument};
            ASSERT_TRUE(runtime.accounts().create_account(1));
            ASSERT_TRUE(runtime.accounts().create_account(2));
            runtime.accounts().fund(1, 20, 5);
            runtime.accounts().fund(2, 10, 1'000);

            const TradingResponse maker = runtime.executor().execute(
                submit_request(201, 1, Side::Sell, 100, 2));
            ASSERT_EQ(maker.result, TradingResult::Accepted);
            const TradingResponse taker = runtime.executor().execute(
                submit_request(
                    202,
                    2,
                    Side::Buy,
                    100,
                    2));

            EXPECT_EQ(taker.result, TradingResult::Accepted);
            EXPECT_EQ(maker.assigned_order_id, 1U);
            EXPECT_EQ(taker.assigned_order_id, 2U);
            const auto& maker_accepted = std::get<OrderAccepted>(
                maker.events.front().payload);
            const auto& taker_accepted = std::get<OrderAccepted>(
                taker.events.front().payload);
            EXPECT_EQ(maker_accepted.order.timestamp, 1);
            EXPECT_EQ(taker_accepted.order.timestamp, 2);
            ASSERT_EQ(taker.events.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<TradeCreated>(
                taker.events[1].payload));
            EXPECT_EQ(runtime.accounts().find_balance(1, 20),
                      (Balance{3, 0}));
            EXPECT_EQ(runtime.accounts().find_balance(1, 10),
                      (Balance{200, 0}));
            EXPECT_EQ(runtime.accounts().find_balance(2, 20),
                      (Balance{2, 0}));
            EXPECT_EQ(runtime.accounts().find_balance(2, 10),
                      (Balance{800, 0}));
            EXPECT_FALSE(runtime.reservations().find(1).has_value());
            EXPECT_FALSE(runtime.reservations().find(2).has_value());
            EXPECT_EQ(runtime.order_book().order_count(), 0U);
            EXPECT_EQ(runtime.ledger().entries().size(), 3U);

            const TradingResponse after_fill = runtime.executor().execute(
                submit_request(203, 2, Side::Buy, 90, 1));
            EXPECT_EQ(after_fill.assigned_order_id, 3U);
            const auto& accepted_after_fill = std::get<OrderAccepted>(
                after_fill.events.front().payload);
            EXPECT_EQ(accepted_after_fill.order.timestamp, 3);
        }

        TEST(TradingRuntimeTest, CancelledOrderIdentityIsNotReused) {
            TradingRuntime runtime{test_instrument};
            ASSERT_TRUE(runtime.accounts().create_account(1));
            runtime.accounts().fund(1, 10, 1'000);

            const TradingResponse first = runtime.executor().execute(
                submit_request(501, 1, Side::Buy, 100, 1));
            ASSERT_EQ(first.assigned_order_id, 1U);
            ASSERT_EQ(
                runtime.executor().execute(TradingRequest{
                    502,
                    1,
                    CancelTradingRequest{1}}).result,
                TradingResult::Cancelled);

            const TradingResponse second = runtime.executor().execute(
                submit_request(503, 1, Side::Buy, 100, 1));
            EXPECT_EQ(second.assigned_order_id, 2U);
            const auto& accepted = std::get<OrderAccepted>(
                second.events.front().payload);
            EXPECT_EQ(accepted.order.timestamp, 2);
        }

        TEST(TradingRuntimeTest, SeparateInstancesDoNotShareState) {
            TradingRuntime first{test_instrument};
            TradingRuntime second{test_instrument};
            ASSERT_TRUE(first.accounts().create_account(1));
            ASSERT_TRUE(second.accounts().create_account(1));
            first.accounts().fund(1, 10, 1'000);
            second.accounts().fund(1, 10, 1'000);

            const TradingResponse first_response = first.executor().execute(
                submit_request(301, 1, Side::Buy, 100, 1));
            const TradingResponse second_response = second.executor().execute(
                submit_request(302, 1, Side::Buy, 100, 1));

            EXPECT_EQ(first_response.assigned_order_id, 1U);
            EXPECT_EQ(second_response.assigned_order_id, 1U);
            EXPECT_TRUE(first.order_book().find_order(1).has_value());
            EXPECT_TRUE(first.reservations().find(1).has_value());
            EXPECT_EQ(first.ledger().entries().size(), 1U);
            EXPECT_TRUE(second.order_book().find_order(1).has_value());
            EXPECT_TRUE(second.reservations().find(1).has_value());
            EXPECT_EQ(second.ledger().entries().size(), 1U);
        }

        TradingResponse execute_before_runtime_destruction() {
            TradingRuntime runtime{test_instrument};
            if (!runtime.accounts().create_account(1)) {
                throw std::logic_error("fresh runtime account already exists");
            }
            runtime.accounts().fund(1, 10, 1'000);
            return runtime.executor().execute(submit_request(
                401,
                1,
                Side::Buy,
                100,
                1));
        }

        TEST(TradingRuntimeTest, ResponseOwnsEventsAfterRuntimeDestruction) {
            const TradingResponse response =
                execute_before_runtime_destruction();

            EXPECT_EQ(response.request_id, 401U);
            EXPECT_EQ(response.result, TradingResult::Accepted);
            EXPECT_EQ(response.assigned_order_id, 1U);
            ASSERT_EQ(response.events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
                response.events.front().payload));
        }
    }  // namespace
}  // namespace exchange
