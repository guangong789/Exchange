#include "exchange/accounting/account_store.hpp"
#include "exchange/accounting/order_reservation_store.hpp"

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        TEST(AccountReservationLifecycleTest, CancellingOneOrderReleasesOnlyItsReservation) {
            AccountStore accounts;
            OrderReservationStore reservations;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);

            ASSERT_EQ(accounts.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(101, 1, 10, 300));
            ASSERT_EQ(accounts.reserve(1, 10, 500), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(102, 1, 10, 500));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{200, 800}));

            const auto cancelled = reservations.find(102);
            ASSERT_TRUE(cancelled.has_value());
            accounts.release(cancelled->account_id,
                             cancelled->asset_id,
                             cancelled->remaining_amount);
            const OrderReservation removed = reservations.remove(102);

            const auto balance = accounts.find_balance(1, 10);
            ASSERT_TRUE(balance.has_value());
            EXPECT_EQ(*balance, (Balance{700, 300}));
            EXPECT_EQ(reservations.find(101),
                      (OrderReservation{1, 10, 300, 300}));
            EXPECT_FALSE(reservations.find(102).has_value());
            EXPECT_EQ(removed, (OrderReservation{1, 10, 500, 500}));

            const auto remaining = reservations.find(101);
            ASSERT_TRUE(remaining.has_value());
            EXPECT_EQ(balance->reserved, remaining->remaining_amount);
        }

        TEST(AccountReservationLifecycleTest, PartialFillThenCancelReleasesOnlyRemainder) {
            AccountStore accounts;
            OrderReservationStore reservations;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(accounts.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(201, 1, 10, 300));

            accounts.consume_reserved(1, 10, 120);
            reservations.consume(201, 120);

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{700, 180}));
            EXPECT_EQ(reservations.find(201),
                      (OrderReservation{1, 10, 300, 180}));

            const auto cancelled = reservations.find(201);
            ASSERT_TRUE(cancelled.has_value());
            accounts.release(cancelled->account_id,
                             cancelled->asset_id,
                             cancelled->remaining_amount);
            static_cast<void>(reservations.remove(201));

            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{880, 0}));
            EXPECT_FALSE(reservations.find(201).has_value());
        }

        TEST(AccountReservationLifecycleTest, FullConsumeRetainsOwnershipUntilExplicitCleanup) {
            AccountStore accounts;
            OrderReservationStore reservations;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 20, 10);
            ASSERT_EQ(accounts.reserve(1, 20, 4), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(301, 1, 20, 4));

            accounts.consume_reserved(1, 20, 4);
            reservations.consume(301, 4);

            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{6, 0}));
            EXPECT_EQ(reservations.find(301),
                      (OrderReservation{1, 20, 4, 0}));

            EXPECT_EQ(reservations.remove(301),
                      (OrderReservation{1, 20, 4, 0}));
            EXPECT_FALSE(reservations.find(301).has_value());
        }

        TEST(AccountReservationLifecycleTest, PartialConsumeKeepsMultipleOrdersIsolated) {
            AccountStore accounts;
            OrderReservationStore reservations;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(accounts.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(401, 1, 10, 300));
            ASSERT_EQ(accounts.reserve(1, 10, 200), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(402, 1, 10, 200));

            accounts.consume_reserved(1, 10, 100);
            reservations.consume(401, 100);

            const auto first = reservations.find(401);
            const auto second = reservations.find(402);
            ASSERT_TRUE(first.has_value());
            ASSERT_TRUE(second.has_value());
            EXPECT_EQ(*first, (OrderReservation{1, 10, 300, 200}));
            EXPECT_EQ(*second, (OrderReservation{1, 10, 200, 200}));
            const auto balance = accounts.find_balance(1, 10);
            ASSERT_TRUE(balance.has_value());
            EXPECT_EQ(balance->reserved, 400);
            EXPECT_EQ(balance->reserved,
                      first->remaining_amount + second->remaining_amount);
        }

        TEST(AccountReservationLifecycleTest, DuplicateCreateRollbackRestoresAccountState) {
            AccountStore accounts;
            OrderReservationStore reservations;
            ASSERT_TRUE(accounts.create_account(1));
            accounts.fund(1, 10, 1'000);
            ASSERT_EQ(accounts.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_TRUE(reservations.create(501, 1, 10, 300));
            const auto balance_before = accounts.find_balance(1, 10);
            const auto record_before = reservations.find(501);

            ASSERT_EQ(accounts.reserve(1, 10, 500), ReserveResult::Success);
            ASSERT_FALSE(reservations.create(501, 2, 20, 500));
            accounts.release(1, 10, 500);

            EXPECT_EQ(accounts.find_balance(1, 10), balance_before);
            EXPECT_EQ(reservations.find(501), record_before);
        }
    }  // namespace
}  // namespace exchange
