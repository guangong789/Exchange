#include "exchange/execution_coordinator.hpp"

#include <array>
#include <initializer_list>
#include <limits>
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

        void expect_order_eq(const Order& actual, const Order& expected) {
            EXPECT_EQ(actual.id, expected.id);
            EXPECT_EQ(actual.side, expected.side);
            EXPECT_EQ(actual.type, expected.type);
            EXPECT_EQ(actual.price, expected.price);
            EXPECT_EQ(actual.quantity, expected.quantity);
            EXPECT_EQ(actual.timestamp, expected.timestamp);
        }

        Amount total_asset(
            const AccountStore& accounts,
            AssetId asset_id,
            std::initializer_list<AccountId> account_ids) {
            Amount total = 0;
            for (const AccountId account_id : account_ids) {
                const auto balance = accounts.find_balance(
                    account_id,
                    asset_id);
                if (balance.has_value()) {
                    total += balance->available + balance->reserved;
                }
            }
            return total;
        }

        class ExecutionCoordinatorTest : public ::testing::Test {
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
        };

        TEST_F(ExecutionCoordinatorTest, AcceptsNonCrossingBuyAndPreservesAcceptedEvent) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const Order order = limit_order(101, Side::Buy, 100, 3, 50);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{1, order});

            EXPECT_EQ(result, SubmitResult::Accepted);
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{700, 300}));
            EXPECT_EQ(reservations.find(101),
                      (OrderReservation{1, 10, 300, 300}));
            ASSERT_TRUE(matching_engine.order_book().find_order(101).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(101), order);
            ASSERT_EQ(events.size(), 1U);
            ASSERT_TRUE(std::holds_alternative<OrderAccepted>(
                events.events().front().payload));
            expect_order_eq(
                std::get<OrderAccepted>(events.events().front().payload).order,
                order);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(
                ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(101, 1, 10, 300)}));
        }

        TEST_F(ExecutionCoordinatorTest, AcceptsNonCrossingSell) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 20, 10);
            const Order order = limit_order(102, Side::Sell, 120, 4, 60);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{1, order});

            EXPECT_EQ(result, SubmitResult::Accepted);
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{6, 4}));
            EXPECT_EQ(reservations.find(102),
                      (OrderReservation{1, 20, 4, 4}));
            ASSERT_TRUE(matching_engine.order_book().find_order(102).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(102), order);
            ASSERT_EQ(events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
                events.events().front().payload));
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(
                ledger.entries()[0].transaction,
                make_reserve_ledger_transaction(102, 1, 20, 4));
        }

        TEST(ExecutionCoordinatorInstrumentTest, RejectsInvalidInstrumentAtConstruction) {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            const std::array<InstrumentContext, 9> invalid_instruments{
                InstrumentContext{0, 10, 1, 1, 1},
                InstrumentContext{20, 0, 1, 1, 1},
                InstrumentContext{10, 10, 1, 1, 1},
                InstrumentContext{20, 10, 0, 1, 1},
                InstrumentContext{20, 10, -1, 1, 1},
                InstrumentContext{20, 10, 1, 0, 1},
                InstrumentContext{20, 10, 1, -1, 1},
                InstrumentContext{20, 10, 1, 1, 0},
                InstrumentContext{20, 10, 1, 1, -1},
            };

            for (const InstrumentContext& instrument : invalid_instruments) {
                EXPECT_THROW(
                    static_cast<void>(ExecutionCoordinator{
                        instrument,
                        accounts,
                        reservations,
                        matching_engine,
                        events,
                        ledger}),
                    std::invalid_argument);
            }

            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(ExecutionCoordinatorInstrumentTest, DerivesRationalBuyReservation) {
            constexpr InstrumentContext instrument{20, 10, 1, 3, 2};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 100);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(115, Side::Buy, 4, 5)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{70, 30}));
            EXPECT_EQ(reservations.find(115),
                      (OrderReservation{1, 10, 30, 30}));
        }

        TEST(ExecutionCoordinatorInstrumentTest, DerivesScaledBaseSellReservation) {
            constexpr InstrumentContext instrument{20, 10, 1'000, 1, 1};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 20, 4'000);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(116, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 20),
                      (Balance{1'000, 3'000}));
            EXPECT_EQ(reservations.find(116),
                      (OrderReservation{1, 20, 3'000, 3'000}));
        }

        TEST(ExecutionCoordinatorInstrumentTest, NonExactConversionLeavesStateUnchanged) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 2};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 100);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        1,
                        limit_order(117, Side::Buy, 3, 2)})),
                std::invalid_argument);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(117).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(120, Side::Buy, 10, 2)}),
                SubmitResult::Accepted);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(ExecutionCoordinatorInstrumentTest, ConversionOverflowLeavesStateUnchanged) {
            constexpr InstrumentContext instrument{20, 10, 1, 2, 1};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 100);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        1,
                        limit_order(
                            118,
                            Side::Buy,
                            std::numeric_limits<Price>::max(),
                            1)})),
                std::overflow_error);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(118).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, InsufficientFundsLeaveAllStateUnchanged) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 200);
            const auto balance_before = accounts.find_balance(1, 10);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{
                    1,
                    limit_order(103, Side::Buy, 100, 3)});

            EXPECT_EQ(result, SubmitResult::InsufficientFunds);
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(103).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, FailedSubmitConsumesNoLedgerSequence) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(700, Side::Buy, 100, 6)}),
                SubmitResult::InsufficientFunds);
            EXPECT_TRUE(ledger.entries().empty());

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(701, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST_F(ExecutionCoordinatorTest, DuplicateReservationRollsBackNewFundsOnly) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_TRUE(reservations.create(104, 2, 20, 500));
            const auto balance_before = accounts.find_balance(1, 10);
            const auto record_before = reservations.find(104);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{
                    1,
                    limit_order(104, Side::Buy, 100, 3)});

            EXPECT_EQ(result, SubmitResult::DuplicateOrder);
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(104), record_before);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, ConversionInputFailuresOccurBeforeMutation) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);
            const std::array<Order, 3> invalid_orders{
                limit_order(105, Side::Buy, 0, 1),
                limit_order(106, Side::Buy, 100, 0),
                limit_order(
                    119,
                    static_cast<Side>(99),
                    100,
                    1),
            };

            for (const Order& order : invalid_orders) {
                EXPECT_THROW(
                    static_cast<void>(coordinator.submit_order(
                        OrderAdmissionRequest{1, order})),
                    std::invalid_argument);
                EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
                EXPECT_FALSE(reservations.find(order.id).has_value());
                EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
                EXPECT_TRUE(events.empty());
                EXPECT_TRUE(ledger.entries().empty());
            }
        }

        TEST_F(ExecutionCoordinatorTest, MatchingValidationFailuresRollBackAdmission) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);

            const std::array<Order, 1> invalid_orders{
                Order{107,
                      Side::Buy,
                      static_cast<OrderType>(99),
                      100,
                      1,
                      0},
            };

            for (const Order& order : invalid_orders) {
                EXPECT_EQ(
                    coordinator.submit_order(
                        OrderAdmissionRequest{1, order}),
                    SubmitResult::InvalidOrder);
                EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
                EXPECT_FALSE(reservations.find(order.id).has_value());
                EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
                EXPECT_TRUE(events.empty());
                EXPECT_TRUE(ledger.entries().empty());
            }
        }

        TEST_F(ExecutionCoordinatorTest, DuplicateLiveMatchingOrderRollsBackAdmission) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const Order resting = limit_order(108, Side::Buy, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(resting).empty());
            events.clear();
            const auto balance_before = accounts.find_balance(1, 10);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{
                    1,
                    limit_order(108, Side::Buy, 90, 1)});

            EXPECT_EQ(result, SubmitResult::InvalidOrder);
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(108).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            ASSERT_TRUE(matching_engine.order_book().find_order(108).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(108), resting);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, CrossingBuyAgainstPureV1MakerIsRejectedWithoutMutation) {
            const Order resting = limit_order(109, Side::Sell, 100, 5);
            ASSERT_TRUE(matching_engine.add_order(resting).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{
                    1,
                    limit_order(110, Side::Buy, 100, 1)});

            EXPECT_EQ(result, SubmitResult::CounterpartyNotAccountBacked);
            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(110).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            EXPECT_FALSE(matching_engine.order_book().find_order(110).has_value());
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, CrossingSellAgainstPureV1MakerIsRejectedWithoutMutation) {
            const Order resting = limit_order(111, Side::Buy, 100, 5);
            ASSERT_TRUE(matching_engine.add_order(resting).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 20, 10);
            const auto balance_before = accounts.find_balance(1, 20);

            const SubmitResult result = coordinator.submit_order(
                OrderAdmissionRequest{
                    1,
                    limit_order(112, Side::Sell, 100, 1)});

            EXPECT_EQ(result, SubmitResult::CounterpartyNotAccountBacked);
            EXPECT_EQ(accounts.find_balance(1, 20), balance_before);
            EXPECT_FALSE(reservations.find(112).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            EXPECT_FALSE(matching_engine.order_book().find_order(112).has_value());
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, TwoAccountsAreAdmittedIndependently) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 1'000);
            accounts.fund(2, 10, 800);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(113, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(114, Side::Buy, 90, 1)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{900, 100}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{710, 90}));
            EXPECT_EQ(reservations.find(113),
                      (OrderReservation{1, 10, 100, 100}));
            EXPECT_EQ(reservations.find(114),
                      (OrderReservation{2, 10, 90, 90}));
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            ASSERT_EQ(events.size(), 1U);
            expect_order_eq(
                std::get<OrderAccepted>(events.events().front().payload).order,
                limit_order(114, Side::Buy, 90, 1));
        }

        TEST(ExecutionCoordinatorDeterminismTest, IdenticalAdmissionsProduceIdenticalState) {
            AccountStore first_accounts;
            AccountStore second_accounts;
            OrderReservationStore first_reservations;
            OrderReservationStore second_reservations;
            EventCollector first_events;
            EventCollector second_events;
            MatchingEngine first_matching{first_events};
            MatchingEngine second_matching{second_events};
            Ledger first_ledger;
            Ledger second_ledger;
            ExecutionCoordinator first{
                test_instrument,
                first_accounts,
                first_reservations,
                first_matching,
                first_events,
                first_ledger};
            ExecutionCoordinator second{
                test_instrument,
                second_accounts,
                second_reservations,
                second_matching,
                second_events,
                second_ledger};

            for (AccountStore* accounts : {&first_accounts, &second_accounts}) {
                ASSERT_TRUE(accounts->create_account(1));
                ASSERT_TRUE(accounts->create_account(2));
                accounts->fund(1, 10, 1'000);
                accounts->fund(2, 20, 500);
            }

            const std::array<OrderAdmissionRequest, 3> requests{
                OrderAdmissionRequest{
                    1,
                    limit_order(201, Side::Buy, 100, 1)},
                OrderAdmissionRequest{
                    2,
                    limit_order(202, Side::Sell, 200, 1)},
                OrderAdmissionRequest{
                    1,
                    limit_order(203, Side::Buy, 90, 1)},
            };

            for (const OrderAdmissionRequest& request : requests) {
                EXPECT_EQ(first.submit_order(request), second.submit_order(request));
            }

            EXPECT_EQ(first_accounts.find_balance(1, 10),
                      second_accounts.find_balance(1, 10));
            EXPECT_EQ(first_accounts.find_balance(2, 20),
                      second_accounts.find_balance(2, 20));
            for (const OrderId order_id : {OrderId{201}, OrderId{202}, OrderId{203}}) {
                EXPECT_EQ(first_reservations.find(order_id),
                          second_reservations.find(order_id));
                const auto first_order =
                    first_matching.order_book().find_order(order_id);
                const auto second_order =
                    second_matching.order_book().find_order(order_id);
                ASSERT_TRUE(first_order.has_value());
                ASSERT_TRUE(second_order.has_value());
                expect_order_eq(*first_order, *second_order);
            }
            EXPECT_EQ(first_matching.order_book().best_bid(),
                      second_matching.order_book().best_bid());
            EXPECT_EQ(first_matching.order_book().best_ask(),
                      second_matching.order_book().best_ask());
            EXPECT_EQ(first_matching.order_book().order_count(),
                      second_matching.order_book().order_count());
            ASSERT_EQ(first_events.size(), second_events.size());
            ASSERT_EQ(first_events.size(), 1U);
            expect_order_eq(
                std::get<OrderAccepted>(first_events.events().front().payload).order,
                std::get<OrderAccepted>(second_events.events().front().payload).order);
            EXPECT_EQ(first_ledger.entries(), second_ledger.entries());
        }

        TEST_F(ExecutionCoordinatorTest, ValidatesCoordinatorBoundaryBeforeMutation) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);
            const Order valid_order = limit_order(301, Side::Buy, 100, 1);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{0, valid_order})),
                std::invalid_argument);
            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(0, Side::Buy, 100, 1)}),
                SubmitResult::InvalidOrder);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, CancelsRestingOrderAndReleasesReservation) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const Order order = limit_order(401, Side::Buy, 100, 3, 70);
            ASSERT_EQ(
                coordinator.submit_order(
                    OrderAdmissionRequest{1, order}),
                SubmitResult::Accepted);

            const CancelResult result = coordinator.cancel_order(1, 401);

            EXPECT_EQ(result, CancelResult::Cancelled);
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{1'000, 0}));
            EXPECT_FALSE(reservations.find(401).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(401).has_value());
            ASSERT_EQ(events.size(), 1U);
            ASSERT_TRUE(std::holds_alternative<OrderCancelled>(
                events.events().front().payload));
            expect_order_eq(
                std::get<OrderCancelled>(events.events().front().payload).order,
                order);
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(
                ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(401, 1, 10, 300)}));
            EXPECT_EQ(
                ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(401, 1, 10, 300)}));
        }

        TEST(ExecutionCoordinatorCancelLedgerTest,
             SellCancelUsesNormalizedReservationSnapshot) {
            constexpr InstrumentContext scaled_instrument{
                20,
                10,
                1'000,
                1,
                1,
            };
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                scaled_instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 20, 4'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(412, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                reservations.find(412),
                (OrderReservation{1, 20, 3'000, 3'000}));

            ASSERT_EQ(
                coordinator.cancel_order(1, 412),
                CancelResult::Cancelled);

            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{4'000, 0}));
            EXPECT_FALSE(reservations.find(412).has_value());
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(
                ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(
                        412,
                        1,
                        20,
                        3'000)}));
            EXPECT_EQ(
                ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(
                        412,
                        1,
                        20,
                        3'000)}));
            ASSERT_EQ(events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderCancelled>(
                events.events().front().payload));
        }

        TEST_F(ExecutionCoordinatorTest, PartialConsumeCancelReleasesOnlyRemainder) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(402, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);
            accounts.consume_reserved(1, 10, 120);
            reservations.consume(402, 120);

            EXPECT_EQ(coordinator.cancel_order(1, 402),
                      CancelResult::Cancelled);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{880, 0}));
            EXPECT_FALSE(reservations.find(402).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(402).has_value());
            ASSERT_EQ(events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderCancelled>(
                events.events().front().payload));
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(
                ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(402, 1, 10, 180)}));
        }

        TEST_F(ExecutionCoordinatorTest, WrongOwnerCannotCancelOrder) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(403, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            const auto balance_before = accounts.find_balance(1, 10);
            const auto reservation_before = reservations.find(403);

            EXPECT_EQ(coordinator.cancel_order(2, 403),
                      CancelResult::NotOwner);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(403), reservation_before);
            EXPECT_TRUE(matching_engine.order_book().find_order(403).has_value());
            EXPECT_TRUE(events.empty());
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);

            EXPECT_EQ(coordinator.cancel_order(1, 403),
                      CancelResult::Cancelled);
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(
                ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(403, 1, 10, 100)}));
        }

        TEST_F(ExecutionCoordinatorTest, MissingReservationReturnsNotFound) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_EQ(coordinator.cancel_order(1, 404),
                      CancelResult::NotFound);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(404).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST_F(ExecutionCoordinatorTest, PureMatchingOrderIsNotAccountCancellable) {
            const Order pure_order = limit_order(405, Side::Buy, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(pure_order).empty());
            ASSERT_FALSE(events.empty());
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_EQ(coordinator.cancel_order(1, 405),
                      CancelResult::NotFound);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(405).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(405).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(405), pure_order);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, CancellingOneOrderLeavesOtherObligationLive) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(406, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(407, Side::Buy, 90, 1)}),
                SubmitResult::Accepted);

            EXPECT_EQ(coordinator.cancel_order(1, 407),
                      CancelResult::Cancelled);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{900, 100}));
            EXPECT_EQ(reservations.find(406),
                      (OrderReservation{1, 10, 100, 100}));
            EXPECT_FALSE(reservations.find(407).has_value());
            EXPECT_TRUE(matching_engine.order_book().find_order(406).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(407).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
        }

        TEST_F(ExecutionCoordinatorTest, CancelRejectsInvalidRequester) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(408, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            const auto balance_before = accounts.find_balance(1, 10);
            const auto reservation_before = reservations.find(408);

            EXPECT_THROW(
                static_cast<void>(coordinator.cancel_order(0, 408)),
                std::invalid_argument);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(408), reservation_before);
            EXPECT_TRUE(matching_engine.order_book().find_order(408).has_value());
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, CancelRejectsInvalidOrderId) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.cancel_order(1, 0)),
                std::invalid_argument);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, MissingMatchingOrderIsFatalAndPreservesEvidence) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(accounts.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(409, 1, 10, 300));
            const auto balance_before = accounts.find_balance(1, 10);
            const auto reservation_before = reservations.find(409);

            EXPECT_THROW(
                static_cast<void>(coordinator.cancel_order(1, 409)),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(409), reservation_before);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            EXPECT_TRUE(events.empty());
            EXPECT_TRUE(ledger.entries().empty());

            ledger.append(
                make_reserve_ledger_transaction(499, 1, 10, 1));
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST_F(ExecutionCoordinatorTest, ZeroRemainingRecordIsFatalAndOrderStaysLive) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(410, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            accounts.consume_reserved(1, 10, 100);
            reservations.consume(410, 100);
            const auto balance_before = accounts.find_balance(1, 10);
            const auto reservation_before = reservations.find(410);

            EXPECT_THROW(
                static_cast<void>(coordinator.cancel_order(1, 410)),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(410), reservation_before);
            EXPECT_TRUE(matching_engine.order_book().find_order(410).has_value());
            EXPECT_TRUE(events.empty());
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST_F(ExecutionCoordinatorTest, ReleaseFailureRetainsMetadataAndCancelEvent) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            const Order order = limit_order(411, Side::Buy, 100, 1);
            ASSERT_EQ(
                coordinator.submit_order(
                    OrderAdmissionRequest{1, order}),
                SubmitResult::Accepted);
            accounts.consume_reserved(1, 10, 100);
            const auto reservation_before = reservations.find(411);

            EXPECT_THROW(
                static_cast<void>(coordinator.cancel_order(1, 411)),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{900, 0}));
            EXPECT_EQ(reservations.find(411), reservation_before);
            EXPECT_FALSE(matching_engine.order_book().find_order(411).has_value());
            ASSERT_EQ(events.size(), 1U);
            ASSERT_TRUE(std::holds_alternative<OrderCancelled>(
                events.events().front().payload));
            expect_order_eq(
                std::get<OrderCancelled>(events.events().front().payload).order,
                order);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(
                ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(411, 1, 10, 100)}));
        }

        TEST(ExecutionCoordinatorCancelDeterminismTest, IdenticalSequencesProduceIdenticalState) {
            AccountStore first_accounts;
            AccountStore second_accounts;
            OrderReservationStore first_reservations;
            OrderReservationStore second_reservations;
            EventCollector first_events;
            EventCollector second_events;
            MatchingEngine first_matching{first_events};
            MatchingEngine second_matching{second_events};
            Ledger first_ledger;
            Ledger second_ledger;
            ExecutionCoordinator first{
                test_instrument,
                first_accounts,
                first_reservations,
                first_matching,
                first_events,
                first_ledger};
            ExecutionCoordinator second{
                test_instrument,
                second_accounts,
                second_reservations,
                second_matching,
                second_events,
                second_ledger};

            for (AccountStore* account_store : {&first_accounts, &second_accounts}) {
                ASSERT_TRUE(account_store->create_account(1));
                ASSERT_TRUE(account_store->create_account(2));
                account_store->fund(1, 10, 1'000);
                account_store->fund(2, 10, 800);
            }

            const std::array<OrderAdmissionRequest, 2> requests{
                OrderAdmissionRequest{
                    1,
                    limit_order(501, Side::Buy, 100, 1)},
                OrderAdmissionRequest{
                    2,
                    limit_order(502, Side::Buy, 90, 1)},
            };
            for (const OrderAdmissionRequest& request : requests) {
                EXPECT_EQ(first.submit_order(request), second.submit_order(request));
            }

            EXPECT_EQ(first.cancel_order(2, 501), second.cancel_order(2, 501));
            EXPECT_EQ(first.cancel_order(1, 999), second.cancel_order(1, 999));
            EXPECT_EQ(first.cancel_order(1, 501), second.cancel_order(1, 501));

            EXPECT_EQ(first_accounts.find_balance(1, 10),
                      second_accounts.find_balance(1, 10));
            EXPECT_EQ(first_accounts.find_balance(2, 10),
                      second_accounts.find_balance(2, 10));
            EXPECT_EQ(first_reservations.find(501),
                      second_reservations.find(501));
            EXPECT_EQ(first_reservations.find(502),
                      second_reservations.find(502));
            EXPECT_EQ(first_matching.order_book().order_count(),
                      second_matching.order_book().order_count());
            EXPECT_EQ(first_matching.order_book().best_bid(),
                      second_matching.order_book().best_bid());
            EXPECT_EQ(first_matching.order_book().best_ask(),
                      second_matching.order_book().best_ask());
            ASSERT_EQ(first_events.size(), second_events.size());
            ASSERT_EQ(first_events.size(), 1U);
            ASSERT_TRUE(std::holds_alternative<OrderCancelled>(
                first_events.events().front().payload));
            ASSERT_TRUE(std::holds_alternative<OrderCancelled>(
                second_events.events().front().payload));
            expect_order_eq(
                std::get<OrderCancelled>(first_events.events().front().payload).order,
                std::get<OrderCancelled>(second_events.events().front().payload).order);
            EXPECT_EQ(first_ledger.entries(), second_ledger.entries());
        }

        TEST_F(ExecutionCoordinatorTest, CrossingBuyClearsAssetsAndCleansCompletedOrders) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 1'000);
            const Order maker = limit_order(601, Side::Sell, 100, 2);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{1, maker}),
                SubmitResult::Accepted);
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto taker_balance = accounts.find_balance(2, 10);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(602, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_NE(accounts.find_balance(1, 20), maker_balance);
            EXPECT_NE(accounts.find_balance(2, 10), taker_balance);
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{3, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{800, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{200, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_FALSE(reservations.find(601).has_value());
            EXPECT_FALSE(reservations.find(602).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(601).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(602).has_value());
            ASSERT_EQ(events.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
                events.events()[0].payload));
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[1].payload));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[1].payload).trade,
                (Trade{602, 601, 100, 2, 0}));
            EXPECT_TRUE(std::holds_alternative<OrderFilled>(
                events.events()[2].payload));
            EXPECT_TRUE(std::holds_alternative<OrderFilled>(
                events.events()[3].payload));
            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_EQ(
                ledger.entries()[0].transaction,
                make_reserve_ledger_transaction(601, 1, 20, 2));
            EXPECT_EQ(
                ledger.entries()[1].transaction,
                make_reserve_ledger_transaction(602, 2, 10, 200));
            EXPECT_EQ(
                ledger.entries()[2].transaction,
                make_trade_ledger_transaction(
                    test_instrument,
                    Trade{602, 601, 100, 2, 0},
                    2,
                    1));
        }

        TEST_F(ExecutionCoordinatorTest, CrossingSellClearsAssetsAndCleansCompletedOrders) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 1'000);
            accounts.fund(2, 20, 5);
            const Order maker = limit_order(603, Side::Buy, 100, 2);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{1, maker}),
                SubmitResult::Accepted);
            const auto maker_balance = accounts.find_balance(1, 10);
            const auto taker_balance = accounts.find_balance(2, 20);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(604, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_NE(accounts.find_balance(1, 10), maker_balance);
            EXPECT_NE(accounts.find_balance(2, 20), taker_balance);
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{800, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{3, 0}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{2, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{200, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_FALSE(reservations.find(603).has_value());
            EXPECT_FALSE(reservations.find(604).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(603).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(604).has_value());
            ASSERT_EQ(events.size(), 4U);
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[1].payload));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[1].payload).trade,
                (Trade{603, 604, 100, 2, 0}));
        }

        TEST_F(ExecutionCoordinatorTest, MultiMakerBuyClearsCumulativeCreditsAndConservesAssets) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 10);
            accounts.fund(2, 10, 1'000);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(605, Side::Sell, 90, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(606, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);
            const auto maker_balance = accounts.find_balance(1, 20);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(607, Side::Buy, 110, 3)}),
                SubmitResult::Accepted);

            EXPECT_NE(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{6, 1}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{710, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{290, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{3, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_FALSE(reservations.find(605).has_value());
            EXPECT_EQ(reservations.find(606),
                      (OrderReservation{1, 20, 3, 1}));
            EXPECT_FALSE(reservations.find(607).has_value());
            ASSERT_EQ(matching_engine.order_book().order_count(), 1U);
            ASSERT_TRUE(matching_engine.order_book().find_order(606).has_value());
            EXPECT_EQ(matching_engine.order_book().find_order(606)->quantity, 1);
            ASSERT_EQ(events.size(), 7U);
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[1].payload));
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[4].payload));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[1].payload).trade,
                (Trade{607, 605, 90, 1, 0}));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[4].payload).trade,
                (Trade{607, 606, 100, 2, 0}));
        }

        TEST_F(ExecutionCoordinatorTest, MultiMakerSellClearsInActualTradeOrder) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            ASSERT_TRUE(accounts.create_account(3));
            accounts.fund(1, 10, 1'000);
            accounts.fund(2, 10, 1'000);
            accounts.fund(3, 20, 10);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(608, Side::Buy, 110, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(609, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);
            const auto first_balance = accounts.find_balance(1, 10);
            const auto second_balance = accounts.find_balance(2, 10);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2, 3});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2, 3});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    3,
                    limit_order(610, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);

            EXPECT_NE(accounts.find_balance(1, 10), first_balance);
            EXPECT_NE(accounts.find_balance(2, 10), second_balance);
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{890, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{800, 0}));
            EXPECT_EQ(accounts.find_balance(3, 20), (Balance{7, 0}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{1, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(accounts.find_balance(3, 10), (Balance{310, 0}));
            EXPECT_EQ(
                total_asset(accounts, 20, {1, 2, 3}),
                base_total_before);
            EXPECT_EQ(
                total_asset(accounts, 10, {1, 2, 3}),
                quote_total_before);
            EXPECT_FALSE(reservations.find(608).has_value());
            EXPECT_FALSE(reservations.find(609).has_value());
            EXPECT_FALSE(reservations.find(610).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(events.size(), 7U);
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[1].payload));
            ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                events.events()[4].payload));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[1].payload).trade,
                (Trade{608, 610, 110, 1, 0}));
            EXPECT_EQ(
                std::get<TradeCreated>(events.events()[4].payload).trade,
                (Trade{609, 610, 100, 2, 0}));
        }

        TEST_F(ExecutionCoordinatorTest, PureV1MakerIsDetectedAndTakerAdmissionRollsBack) {
            const Order maker = limit_order(611, Side::Sell, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);
            const auto balance_before = accounts.find_balance(1, 10);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(612, Side::Buy, 100, 1)}),
                SubmitResult::CounterpartyNotAccountBacked);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_FALSE(reservations.find(612).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(611).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(611), maker);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, WrongMakerReservationAssetIsFatalAndReadOnly) {
            const Order maker = limit_order(613, Side::Sell, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 100);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(accounts.reserve(1, 10, 100), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(613, 1, 10, 100));
            const auto maker_balance = accounts.find_balance(1, 10);
            const auto maker_reservation = reservations.find(613);
            const auto taker_balance = accounts.find_balance(2, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(614, Side::Buy, 100, 1)})),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 10), maker_balance);
            EXPECT_EQ(accounts.find_balance(2, 10), taker_balance);
            EXPECT_EQ(reservations.find(613), maker_reservation);
            EXPECT_FALSE(reservations.find(614).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(613).has_value());
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, MakerOrderProjectionRejectsInsufficientRemaining) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(615, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);
            reservations.consume(615, 1);
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto maker_reservation = reservations.find(615);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(616, Side::Buy, 100, 2)})),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(reservations.find(615), maker_reservation);
            EXPECT_FALSE(reservations.find(616).has_value());
            ASSERT_EQ(matching_engine.order_book().find_order(615)->quantity, 2);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, AggregateReservedProjectionRejectsCumulativeDeficit) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(617, Side::Sell, 90, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(618, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            accounts.consume_reserved(1, 20, 1);
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto first_reservation = reservations.find(617);
            const auto second_reservation = reservations.find(618);
            const auto ledger_before = ledger.entries();

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(619, Side::Buy, 100, 2)})),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(reservations.find(617), first_reservation);
            EXPECT_EQ(reservations.find(618), second_reservation);
            EXPECT_FALSE(reservations.find(619).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            EXPECT_TRUE(events.empty());
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST_F(ExecutionCoordinatorTest, FullyFilledBuyReleasesPriceImprovementResidual) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(620, Side::Sell, 90, 2)}),
                SubmitResult::Accepted);
            const auto taker_balance = accounts.find_balance(2, 10);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(621, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_NE(accounts.find_balance(2, 10), taker_balance);
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{320, 0}));
            EXPECT_FALSE(reservations.find(621).has_value());
            EXPECT_FALSE(reservations.find(620).has_value());
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{3, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{180, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            ASSERT_EQ(events.size(), 4U);
            ASSERT_EQ(ledger.entries().size(), 4U);
            EXPECT_EQ(
                ledger.entries()[1].transaction,
                make_reserve_ledger_transaction(621, 2, 10, 200));
            EXPECT_EQ(
                ledger.entries()[2].transaction,
                make_trade_ledger_transaction(
                    test_instrument,
                    Trade{621, 620, 90, 2, 0},
                    2,
                    1));
            EXPECT_EQ(
                ledger.entries()[3].transaction,
                make_release_ledger_transaction(621, 2, 10, 20));
        }

        TEST_F(ExecutionCoordinatorTest, FullyFilledSellRequiresZeroProjectedRemaining) {
            const Order maker = limit_order(622, Side::Sell, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 2);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(accounts.reserve(1, 20, 2), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(622, 1, 20, 2));
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto maker_reservation = reservations.find(622);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(623, Side::Buy, 100, 1)})),
                std::logic_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(reservations.find(622), maker_reservation);
            EXPECT_FALSE(reservations.find(623).has_value());
            ASSERT_EQ(matching_engine.order_book().find_order(622)->quantity, 1);
            EXPECT_TRUE(events.empty());
        }

        TEST(ExecutionCoordinatorPreflightConversionTest, FailureRollsBackTakerBeforeMatching) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 2};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            const Order maker = limit_order(632, Side::Sell, 3, 2);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 2);
            accounts.fund(2, 10, 20);
            ASSERT_EQ(accounts.reserve(1, 20, 2), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(632, 1, 20, 2));
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto maker_reservation = reservations.find(632);
            const auto taker_balance = accounts.find_balance(2, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(633, Side::Buy, 4, 2)})),
                std::invalid_argument);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(accounts.find_balance(2, 10), taker_balance);
            EXPECT_EQ(reservations.find(632), maker_reservation);
            EXPECT_FALSE(reservations.find(633).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(632).has_value());
            expect_order_eq(
                *matching_engine.order_book().find_order(632), maker);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, PartialImmediateBuyThenCancelReleasesExactRemainder) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 10);
            accounts.fund(2, 10, 1'000);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(624, Side::Sell, 100, 5)}),
                SubmitResult::Accepted);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(625, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);
            EXPECT_EQ(reservations.find(624),
                      (OrderReservation{1, 20, 5, 3}));
            EXPECT_FALSE(reservations.find(625).has_value());
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{5, 3}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{800, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{200, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            ASSERT_EQ(matching_engine.order_book().find_order(624)->quantity, 3);

            ASSERT_EQ(coordinator.cancel_order(1, 624), CancelResult::Cancelled);
            ASSERT_EQ(ledger.entries().size(), 4U);
            EXPECT_EQ(
                ledger.entries()[3],
                (LedgerEntry{
                    4,
                    make_release_ledger_transaction(624, 1, 20, 3)}));
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(626, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);
            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(627, Side::Buy, 100, 5)}),
                SubmitResult::Accepted);
            EXPECT_FALSE(reservations.find(626).has_value());
            EXPECT_EQ(reservations.find(627),
                      (OrderReservation{2, 10, 500, 300}));
            ASSERT_EQ(matching_engine.order_book().find_order(627)->quantity, 3);
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{6, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{300, 300}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{400, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{4, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);

            EXPECT_EQ(coordinator.cancel_order(2, 627), CancelResult::Cancelled);
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{600, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_FALSE(reservations.find(627).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(627).has_value());
            ASSERT_EQ(ledger.entries().size(), 8U);
            EXPECT_EQ(
                ledger.entries()[7],
                (LedgerEntry{
                    8,
                    make_release_ledger_transaction(627, 2, 10, 300)}));
        }

        TEST_F(ExecutionCoordinatorTest, ThreeLevelBuyReleasesExactPriceImprovementResidual) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 3);
            accounts.fund(2, 10, 1'000);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});
            for (const Order& maker : {
                     limit_order(634, Side::Sell, 90, 1),
                     limit_order(635, Side::Sell, 95, 1),
                     limit_order(636, Side::Sell, 100, 1)}) {
                ASSERT_EQ(
                    coordinator.submit_order(OrderAdmissionRequest{1, maker}),
                    SubmitResult::Accepted);
            }

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(637, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{0, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{715, 0}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{285, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{3, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            for (const OrderId id : {
                     OrderId{634}, OrderId{635}, OrderId{636}, OrderId{637}}) {
                EXPECT_FALSE(reservations.find(id).has_value());
            }
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(events.size(), 10U);
            ASSERT_EQ(ledger.entries().size(), 8U);
            EXPECT_EQ(
                ledger.entries()[3].transaction,
                make_reserve_ledger_transaction(637, 2, 10, 300));
            const std::array<Trade, 3> expected_trades{
                Trade{637, 634, 90, 1, 0},
                Trade{637, 635, 95, 1, 0},
                Trade{637, 636, 100, 1, 0},
            };
            for (std::size_t index = 0;
                 index < expected_trades.size();
                 ++index) {
                EXPECT_EQ(
                    ledger.entries()[4 + index].transaction,
                    make_trade_ledger_transaction(
                        test_instrument,
                        expected_trades[index],
                        2,
                        1));
            }
            EXPECT_EQ(
                ledger.entries()[7].transaction,
                make_release_ledger_transaction(637, 2, 10, 15));
        }

        TEST_F(ExecutionCoordinatorTest, PartialImmediateSellRestsWithBaseRemainder) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 500);
            accounts.fund(2, 20, 10);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(638, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(639, Side::Sell, 100, 5)}),
                SubmitResult::Accepted);

            EXPECT_FALSE(reservations.find(638).has_value());
            EXPECT_EQ(reservations.find(639),
                      (OrderReservation{2, 20, 5, 3}));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{300, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{5, 3}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{2, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{200, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            ASSERT_TRUE(matching_engine.order_book().find_order(639).has_value());
            EXPECT_EQ(matching_engine.order_book().find_order(639)->quantity, 3);
            EXPECT_EQ(matching_engine.order_book().order_count(), 1U);
            ASSERT_EQ(events.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<OrderPartiallyFilled>(
                events.events()[3].payload));
            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                ledger.entries()[1].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                ledger.entries()[2].transaction.metadata));
        }

        TEST_F(ExecutionCoordinatorTest, FullyFilledBuyMakerReleasesPositiveResidualGenerically) {
            const Order maker = limit_order(640, Side::Buy, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 10, 150);
            accounts.fund(2, 20, 1);
            ASSERT_EQ(accounts.reserve(1, 10, 150), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(640, 1, 10, 150));
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(641, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{50, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{0, 0}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{1, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{100, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_FALSE(reservations.find(640).has_value());
            EXPECT_FALSE(reservations.find(641).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_EQ(
                ledger.entries()[0].transaction,
                make_reserve_ledger_transaction(641, 2, 20, 1));
            EXPECT_EQ(
                ledger.entries()[1].transaction,
                make_trade_ledger_transaction(
                    test_instrument,
                    Trade{640, 641, 100, 1, 0},
                    1,
                    2));
            EXPECT_EQ(
                ledger.entries()[2].transaction,
                make_release_ledger_transaction(640, 1, 10, 50));
        }

        TEST_F(ExecutionCoordinatorTest, NonExecutablePureV1OrderDoesNotBlockAdmission) {
            const Order pure_order = limit_order(642, Side::Sell, 120, 1);
            ASSERT_TRUE(matching_engine.add_order(pure_order).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 500);
            const Order incoming = limit_order(643, Side::Buy, 100, 1);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{1, incoming}),
                SubmitResult::Accepted);

            EXPECT_EQ(reservations.find(643),
                      (OrderReservation{1, 10, 100, 100}));
            ASSERT_TRUE(matching_engine.order_book().find_order(642).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(643).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            ASSERT_EQ(events.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
                events.events().front().payload));
        }

        TEST_F(ExecutionCoordinatorTest, ExistingCreditDestinationsAccumulateRuntimeCredits) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 1);
            accounts.fund(1, 10, 60);
            accounts.fund(2, 10, 200);
            accounts.fund(2, 20, 50);
            const Amount base_total_before = total_asset(accounts, 20, {1, 2});
            const Amount quote_total_before = total_asset(accounts, 10, {1, 2});
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(650, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(651, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{160, 0}));
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{51, 0}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{0, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{100, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1, 2}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1, 2}), quote_total_before);
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
        }

        TEST_F(ExecutionCoordinatorTest, BuyerBaseCreditOverflowFailsBeforeMatching) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 1);
            accounts.fund(2, 10, 200);
            accounts.fund(2, 20, maximum);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(652, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto maker_reservation = reservations.find(652);
            const auto taker_quote = accounts.find_balance(2, 10);
            const auto ledger_before = ledger.entries();

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(653, Side::Buy, 100, 1)})),
                std::overflow_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(accounts.find_balance(2, 10), taker_quote);
            EXPECT_EQ(accounts.find_balance(2, 20), (Balance{maximum, 0}));
            EXPECT_EQ(reservations.find(652), maker_reservation);
            EXPECT_FALSE(reservations.find(653).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(652).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(653).has_value());
            EXPECT_TRUE(events.empty());
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST_F(ExecutionCoordinatorTest, SellerQuoteCreditOverflowFailsBeforeMatching) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 1);
            accounts.fund(1, 10, maximum);
            accounts.fund(2, 10, 200);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(654, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            const auto maker_base = accounts.find_balance(1, 20);
            const auto maker_quote = accounts.find_balance(1, 10);
            const auto maker_reservation = reservations.find(654);
            const auto taker_quote = accounts.find_balance(2, 10);
            const auto ledger_before = ledger.entries();

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(655, Side::Buy, 100, 1)})),
                std::overflow_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_base);
            EXPECT_EQ(accounts.find_balance(1, 10), maker_quote);
            EXPECT_EQ(accounts.find_balance(2, 10), taker_quote);
            EXPECT_EQ(reservations.find(654), maker_reservation);
            EXPECT_FALSE(reservations.find(655).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(654).has_value());
            EXPECT_TRUE(events.empty());
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST_F(ExecutionCoordinatorTest, CumulativeBuyerCreditOverflowFailsOnLaterTrade) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 4);
            accounts.fund(2, 10, 1'000);
            accounts.fund(2, 20, maximum - 3);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(656, Side::Sell, 90, 2)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(657, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);
            const auto maker_balance = accounts.find_balance(1, 20);
            const auto first_reservation = reservations.find(656);
            const auto second_reservation = reservations.find(657);
            const auto taker_quote = accounts.find_balance(2, 10);
            const auto ledger_before = ledger.entries();

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(658, Side::Buy, 100, 4)})),
                std::overflow_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_balance);
            EXPECT_EQ(accounts.find_balance(2, 10), taker_quote);
            EXPECT_EQ(accounts.find_balance(2, 20),
                      (Balance{maximum - 3, 0}));
            EXPECT_EQ(reservations.find(656), first_reservation);
            EXPECT_EQ(reservations.find(657), second_reservation);
            EXPECT_FALSE(reservations.find(658).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            EXPECT_TRUE(events.empty());
            EXPECT_EQ(ledger.entries(), ledger_before);
        }

        TEST_F(ExecutionCoordinatorTest, CumulativeSellerCreditOverflowFailsOnLaterTrade) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 2);
            accounts.fund(1, 10, maximum - 150);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(659, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(660, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            const auto maker_base = accounts.find_balance(1, 20);
            const auto maker_quote = accounts.find_balance(1, 10);
            const auto first_reservation = reservations.find(659);
            const auto second_reservation = reservations.find(660);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(661, Side::Buy, 100, 2)})),
                std::overflow_error);

            EXPECT_EQ(accounts.find_balance(1, 20), maker_base);
            EXPECT_EQ(accounts.find_balance(1, 10), maker_quote);
            EXPECT_EQ(reservations.find(659), first_reservation);
            EXPECT_EQ(reservations.find(660), second_reservation);
            EXPECT_FALSE(reservations.find(661).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 2U);
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, SelfTradeClearsConsumeAndCreditFieldsIndependently) {
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            accounts.fund(1, 20, 5);
            const Amount base_total_before = total_asset(accounts, 20, {1});
            const Amount quote_total_before = total_asset(accounts, 10, {1});
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(662, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(663, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{1'000, 0}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{5, 0}));
            EXPECT_EQ(total_asset(accounts, 20, {1}), base_total_before);
            EXPECT_EQ(total_asset(accounts, 10, {1}), quote_total_before);
            EXPECT_FALSE(reservations.find(662).has_value());
            EXPECT_FALSE(reservations.find(663).has_value());
            EXPECT_EQ(matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(events.size(), 4U);
        }

        TEST_F(ExecutionCoordinatorTest, MissingConsumptionAssetRowRemainsFatal) {
            const Order maker = limit_order(664, Side::Sell, 100, 1);
            ASSERT_TRUE(matching_engine.add_order(maker).empty());
            events.clear();
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(2, 10, 200);
            ASSERT_TRUE(reservations.create(664, 1, 20, 1));
            const auto maker_reservation = reservations.find(664);
            const auto taker_balance = accounts.find_balance(2, 10);

            EXPECT_THROW(
                static_cast<void>(coordinator.submit_order(
                    OrderAdmissionRequest{
                        2,
                        limit_order(665, Side::Buy, 100, 1)})),
                std::logic_error);

            EXPECT_FALSE(accounts.find_balance(1, 20).has_value());
            EXPECT_EQ(accounts.find_balance(2, 10), taker_balance);
            EXPECT_EQ(reservations.find(664), maker_reservation);
            EXPECT_FALSE(reservations.find(665).has_value());
            ASSERT_TRUE(matching_engine.order_book().find_order(664).has_value());
            EXPECT_FALSE(matching_engine.order_book().find_order(665).has_value());
            EXPECT_TRUE(events.empty());
        }

        TEST_F(ExecutionCoordinatorTest, DuplicateOrderIdCannotEnterPlanOnBothSides) {
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 20, 5);
            accounts.fund(2, 10, 500);
            ASSERT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(628, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            const auto maker_reservation = reservations.find(628);

            EXPECT_EQ(
                coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(628, Side::Buy, 100, 1)}),
                SubmitResult::DuplicateOrder);

            EXPECT_EQ(reservations.find(628), maker_reservation);
            ASSERT_TRUE(matching_engine.order_book().find_order(628).has_value());
            EXPECT_TRUE(events.empty());
        }

        TEST(ExecutionCoordinatorPreflightDeterminismTest, IdenticalCrossingsLeaveIdenticalState) {
            AccountStore first_accounts;
            AccountStore second_accounts;
            OrderReservationStore first_reservations;
            OrderReservationStore second_reservations;
            EventCollector first_events;
            EventCollector second_events;
            MatchingEngine first_matching{first_events};
            MatchingEngine second_matching{second_events};
            Ledger first_ledger;
            Ledger second_ledger;
            ExecutionCoordinator first{
                test_instrument,
                first_accounts,
                first_reservations,
                first_matching,
                first_events,
                first_ledger};
            ExecutionCoordinator second{
                test_instrument,
                second_accounts,
                second_reservations,
                second_matching,
                second_events,
                second_ledger};

            for (AccountStore* store : {&first_accounts, &second_accounts}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 20, 5);
                store->fund(2, 10, 500);
            }
            for (ExecutionCoordinator* coordinator : {&first, &second}) {
                ASSERT_EQ(
                    coordinator->submit_order(OrderAdmissionRequest{
                        1,
                        limit_order(629, Side::Sell, 90, 1)}),
                    SubmitResult::Accepted);
                ASSERT_EQ(
                    coordinator->submit_order(OrderAdmissionRequest{
                        1,
                        limit_order(630, Side::Sell, 100, 1)}),
                    SubmitResult::Accepted);
            }

            EXPECT_EQ(
                first.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(631, Side::Buy, 100, 3)}),
                second.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(631, Side::Buy, 100, 3)}));

            EXPECT_EQ(first_accounts.find_balance(1, 20),
                      second_accounts.find_balance(1, 20));
            EXPECT_EQ(first_accounts.find_balance(2, 10),
                      second_accounts.find_balance(2, 10));
            for (const OrderId id : {OrderId{629}, OrderId{630}, OrderId{631}}) {
                EXPECT_EQ(first_reservations.find(id),
                          second_reservations.find(id));
            }
            EXPECT_EQ(first_matching.order_book().order_count(),
                      second_matching.order_book().order_count());
            ASSERT_EQ(first_events.size(), second_events.size());
            ASSERT_EQ(first_events.size(), 7U);
            for (const std::size_t index : {std::size_t{1}, std::size_t{4}}) {
                ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                    first_events.events()[index].payload));
                ASSERT_TRUE(std::holds_alternative<TradeCreated>(
                    second_events.events()[index].payload));
                EXPECT_EQ(
                    std::get<TradeCreated>(
                        first_events.events()[index].payload).trade,
                    std::get<TradeCreated>(
                        second_events.events()[index].payload).trade);
            }
            EXPECT_EQ(first_ledger.entries(), second_ledger.entries());
        }
    }  // namespace
}  // namespace exchange
