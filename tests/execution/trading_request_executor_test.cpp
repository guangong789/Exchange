#include "exchange/execution/trading_request_executor.hpp"

#include <stdexcept>
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

        Order limit_order(
            OrderId id,
            Side side,
            Price price,
            Quantity quantity,
            Timestamp timestamp = 0) {
            return Order{id, side, OrderType::Limit, price, quantity, timestamp};
        }

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

        TradingRequest cancel_request(
            RequestId request_id,
            AccountId account_id,
            OrderId order_id) {
            return TradingRequest{
                request_id,
                account_id,
                CancelTradingRequest{order_id}};
        }

        class TradingRequestExecutorTest : public ::testing::Test {
        protected:
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                test_instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ExecutionSequencer sequencer;
            TradingRequestExecutor executor{
                test_instrument,
                coordinator,
                events,
                sequencer};
        };

        TEST_F(TradingRequestExecutorTest,
               FundedAccountSubmitsRestingOrderAndPreservesRequestId) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);

            const TradingResponse response = executor.execute(submit_request(
                101,
                1,
                Side::Buy,
                100,
                3));

            EXPECT_EQ(response.request_id, 101U);
            EXPECT_EQ(response.result, TradingResult::Accepted);
            EXPECT_EQ(response.assigned_order_id, 1U);
            ASSERT_EQ(response.events.size(), 1U);
            const auto& accepted = std::get<OrderAccepted>(
                response.events.front().payload);
            EXPECT_EQ(accepted.order.id, 1U);
            EXPECT_EQ(accepted.order.timestamp, 1);
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{700, 300}));
            EXPECT_EQ(reservations.find(1),
                      (OrderReservation{1, 10, 300, 300}));
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_EQ(ledger.entries().size(), 1U);
        }

        TEST_F(TradingRequestExecutorTest,
               InsufficientFundsIsNormalRejectionWithoutMutation) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 200);
            const auto balance_before = accounts.find_balance(1, 10);

            const TradingResponse response = executor.execute(submit_request(
                202,
                1,
                Side::Buy,
                100,
                3));

            EXPECT_EQ(response.request_id, 202U);
            EXPECT_EQ(response.result, TradingResult::InsufficientFunds);
            EXPECT_TRUE(response.events.empty());
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(response.assigned_order_id.has_value());
            EXPECT_FALSE(reservations.find(1).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(ledger.entries().empty());

            accounts.fund(1, 10, 1'000);
            const TradingResponse next = executor.execute(
                submit_request(204, 1, Side::Buy, 100, 1));
            EXPECT_EQ(next.assigned_order_id, 2U);
        }

        TEST_F(TradingRequestExecutorTest,
               MissingSubmitAccountPreservesRequestIdWithoutMutation) {
            const TradingResponse response = executor.execute(submit_request(
                203,
                99,
                Side::Buy,
                100,
                1));

            EXPECT_EQ(response.request_id, 203U);
            EXPECT_EQ(response.result, TradingResult::AccountNotFound);
            EXPECT_TRUE(response.events.empty());
            EXPECT_FALSE(accounts.contains_account(99));
            EXPECT_FALSE(accounts.find_balance(99, 10).has_value());
            EXPECT_FALSE(response.assigned_order_id.has_value());
            EXPECT_FALSE(reservations.find(1).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(ledger.entries().empty());

            ASSERT_TRUE(accounts.create_account(99));
            accounts.fund(99, 10, 1'000);
            const TradingResponse next = executor.execute(
                submit_request(205, 99, Side::Buy, 100, 1));
            EXPECT_EQ(next.assigned_order_id, 2U);
        }

        TEST_F(TradingRequestExecutorTest,
               TwoAccountsExecuteTradeThroughExecutor) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 1'000);

            const TradingResponse maker = executor.execute(submit_request(
                301,
                1,
                Side::Sell,
                100,
                2));
            const TradingResponse taker = executor.execute(submit_request(
                302,
                2,
                Side::Buy,
                100,
                2));

            EXPECT_EQ(maker.result, TradingResult::Accepted);
            EXPECT_EQ(maker.assigned_order_id, 1U);
            ASSERT_EQ(maker.events.size(), 1U);
            EXPECT_EQ(taker.request_id, 302U);
            EXPECT_EQ(taker.result, TradingResult::Accepted);
            EXPECT_EQ(taker.assigned_order_id, 2U);
            ASSERT_EQ(taker.events.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<TradeCreated>(
                taker.events[1].payload));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{3, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{200, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{800, 0}));
            EXPECT_FALSE(reservations.find(1).has_value());
            EXPECT_FALSE(reservations.find(2).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_EQ(ledger.entries().size(), 3U);
        }

        TEST_F(TradingRequestExecutorTest,
               OwnerCancellationSucceedsAndReturnsCancellationEvent) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                executor.execute(submit_request(
                    401,
                    1,
                    Side::Buy,
                    100,
                    3)).result,
                TradingResult::Accepted);

            const TradingResponse response = executor.execute(
                cancel_request(402, 1, 1));

            EXPECT_EQ(response.request_id, 402U);
            EXPECT_EQ(response.result, TradingResult::Cancelled);
            ASSERT_EQ(response.events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderCancelled>(
                response.events.front().payload));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{1'000, 0}));
            EXPECT_FALSE(reservations.find(1).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_EQ(ledger.entries().size(), 2U);
        }

        TEST_F(TradingRequestExecutorTest,
               NonOwnerCancellationIsNormalRejection) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                executor.execute(submit_request(
                    501,
                    1,
                    Side::Buy,
                    100,
                    3)).result,
                TradingResult::Accepted);
            const auto owner_balance_before = accounts.find_balance(1, 10);

            const TradingResponse response = executor.execute(
                cancel_request(502, 2, 1));

            EXPECT_EQ(response.request_id, 502U);
            EXPECT_EQ(response.result, TradingResult::CancelNotOwner);
            EXPECT_TRUE(response.events.empty());
            EXPECT_EQ(accounts.find_balance(1, 10), owner_balance_before);
            EXPECT_TRUE(reservations.find(1).has_value());
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_EQ(ledger.entries().size(), 1U);
        }

        TEST_F(TradingRequestExecutorTest,
               MissingOrderCancellationMapsWithoutThrowing) {
            ASSERT_TRUE(accounts.create_account(1));

            const TradingResponse response = executor.execute(
                cancel_request(601, 1, 999));

            EXPECT_EQ(response.request_id, 601U);
            EXPECT_EQ(response.result, TradingResult::CancelNotFound);
            EXPECT_TRUE(response.events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(TradingRequestExecutorTest,
               MissingCancelAccountPreservesRequestIdWithoutMutation) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                executor.execute(submit_request(
                    602,
                    1,
                    Side::Buy,
                    100,
                    1)).result,
                TradingResult::Accepted);
            const auto balance_before = accounts.find_balance(1, 10);
            const auto reservation_before = reservations.find(1);
            const std::size_t ledger_size_before = ledger.entries().size();

            const TradingResponse response = executor.execute(
                cancel_request(603, 99, 1));

            EXPECT_EQ(response.request_id, 603U);
            EXPECT_EQ(response.result, TradingResult::AccountNotFound);
            EXPECT_TRUE(response.events.empty());
            EXPECT_FALSE(accounts.contains_account(99));
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(1), reservation_before);
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_EQ(ledger.entries().size(), ledger_size_before);
        }

        TEST_F(TradingRequestExecutorTest,
               UnexpectedInternalFailureStillPropagates) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 100);
            ASSERT_EQ(
                accounts.reserve(1, 10, 100),
                ReserveResult::Success);
            ASSERT_TRUE(reservations.create(43, 1, 10, 100));

            EXPECT_THROW(
                static_cast<void>(executor.execute(
                    cancel_request(604, 1, 43))),
                std::logic_error);

            EXPECT_FALSE(matching_engine.order_book().find_order(43).has_value());
            EXPECT_TRUE(reservations.find(43).has_value());
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{0, 100}));
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(TradingRequestExecutorTest,
               RepeatedRequestIdIsNotDeduplicated) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const TradingResponse first = executor.execute(submit_request(
                701,
                1,
                Side::Buy,
                100,
                1));
            const TradingResponse second = executor.execute(submit_request(
                701,
                1,
                Side::Buy,
                90,
                1));

            EXPECT_EQ(first.result, TradingResult::Accepted);
            EXPECT_EQ(first.assigned_order_id, 1U);
            EXPECT_EQ(second.request_id, 701U);
            EXPECT_EQ(second.result, TradingResult::Accepted);
            EXPECT_EQ(second.assigned_order_id, 2U);
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            EXPECT_EQ(ledger.entries().size(), 2U);
        }

        TEST_F(TradingRequestExecutorTest,
               InvalidOrderDoesNotConsumeSequence) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);

            const TradingResponse invalid = executor.execute(submit_request(
                702,
                1,
                Side::Buy,
                0,
                1));
            const TradingResponse accepted = executor.execute(submit_request(
                703,
                1,
                Side::Buy,
                100,
                1));

            EXPECT_EQ(invalid.request_id, 702U);
            EXPECT_EQ(invalid.result, TradingResult::InvalidOrder);
            EXPECT_TRUE(invalid.events.empty());
            EXPECT_FALSE(invalid.assigned_order_id.has_value());
            EXPECT_EQ(accepted.assigned_order_id, 1U);
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            EXPECT_EQ(ledger.entries().size(), 1U);
        }

        TEST_F(TradingRequestExecutorTest,
               InvalidSubmitMetadataDoesNotConsumeSequence) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);

            const TradingResponse invalid = executor.execute(
                TradingRequest{
                    0,
                    1,
                    SubmitTradingRequest{Side::Buy, 100, 1}});
            const TradingResponse accepted = executor.execute(
                submit_request(705, 1, Side::Buy, 100, 1));

            EXPECT_EQ(invalid.result, TradingResult::InvalidRequest);
            EXPECT_FALSE(invalid.assigned_order_id.has_value());
            EXPECT_EQ(accepted.assigned_order_id, 1U);
        }

        TEST_F(TradingRequestExecutorTest,
               ExistingInternalIdentityCollisionMapsDuplicate) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                accounts.reserve(1, 10, 100),
                ReserveResult::Success);
            ASSERT_TRUE(reservations.create(1, 1, 10, 100));
            ASSERT_TRUE(matching_engine.add_order(
                limit_order(1, Side::Buy, 100, 1, 50)).empty());
            events.clear();

            const TradingResponse duplicate = executor.execute(
                submit_request(704, 1, Side::Buy, 90, 1));

            EXPECT_EQ(duplicate.result, TradingResult::DuplicateOrder);
            EXPECT_FALSE(duplicate.assigned_order_id.has_value());
            EXPECT_TRUE(duplicate.events.empty());
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{900, 100}));
            EXPECT_EQ(reservations.find(1),
                      (OrderReservation{1, 10, 100, 100}));
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(TradingRequestExecutorTest,
               CounterpartyWithoutAccountBackingMapsAsBusinessRejection) {
            const Order pure_matching_order =
                limit_order(61, Side::Sell, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(pure_matching_order).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);

            const TradingResponse response = executor.execute(submit_request(
                801,
                1,
                Side::Buy,
                100,
                1));

            EXPECT_EQ(response.request_id, 801U);
            EXPECT_EQ(
                response.result,
                TradingResult::CounterpartyNotAccountBacked);
            EXPECT_TRUE(response.events.empty());
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{1'000, 0}));
            EXPECT_FALSE(reservations.find(1).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(TradingRequestExecutorTest,
               InvalidMetadataReturnsStableResponseAndClearsStaleEvents) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                executor.execute(submit_request(
                    901,
                    1,
                    Side::Buy,
                    100,
                    1)).result,
                TradingResult::Accepted);
            ASSERT_FALSE(events.empty());

            const TradingResponse zero_request_id = executor.execute(
                cancel_request(0, 1, 71));
            const TradingResponse zero_account_id = executor.execute(
                cancel_request(902, 0, 71));

            EXPECT_EQ(zero_request_id.request_id, 0U);
            EXPECT_EQ(zero_request_id.result, TradingResult::InvalidRequest);
            EXPECT_TRUE(zero_request_id.events.empty());
            EXPECT_EQ(zero_account_id.request_id, 902U);
            EXPECT_EQ(zero_account_id.result, TradingResult::InvalidRequest);
            EXPECT_TRUE(zero_account_id.events.empty());
            EXPECT_TRUE(matching_engine.order_book().find_order(1).has_value());
            EXPECT_EQ(ledger.entries().size(), 1U);
        }
    }  // namespace
}  // namespace exchange
