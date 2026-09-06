#include "exchange/accounting/order_reservation_store.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        TEST(OrderReservationStoreTest, CreateStoresOriginalAndRemainingAmount) {
            OrderReservationStore store;

            EXPECT_TRUE(store.create(101, 1, 10, 300));
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreTest, DuplicateOrderDoesNotOverwriteRecord) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            EXPECT_FALSE(store.create(101, 2, 20, 500));
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreTest, FindReturnsValueSnapshot) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            auto snapshot = store.find(101);
            ASSERT_TRUE(snapshot.has_value());
            snapshot->remaining_amount = 0;

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreTest, MissingFindReturnsNullopt) {
            OrderReservationStore store;

            EXPECT_FALSE(store.find(101).has_value());
            EXPECT_FALSE(store.find(101).has_value());
        }

        TEST(OrderReservationStoreTest, RemoveReturnsExactPositiveRecordAndErasesMetadata) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            const OrderReservation removed = store.remove(101);

            EXPECT_EQ(removed, (OrderReservation{1, 10, 300, 300}));
            EXPECT_FALSE(store.find(101).has_value());
        }

        TEST(OrderReservationStoreTest, MissingRemoveLeavesOtherRecordsUnchanged) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            EXPECT_THROW(static_cast<void>(store.remove(102)), std::out_of_range);
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreTest, RejectsInvalidOrderIdForAllOperations) {
            OrderReservationStore store;

            EXPECT_THROW(static_cast<void>(store.create(0, 1, 10, 300)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.find(0)), std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.remove(0)),
                         std::invalid_argument);
        }

        TEST(OrderReservationStoreTest, CreateRejectsInvalidOwnerAssetAndAmount) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            EXPECT_THROW(static_cast<void>(store.create(102, 0, 10, 300)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.create(102, 1, 0, 300)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.create(102, 1, 10, 0)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.create(102, 1, 10, -1)),
                         std::invalid_argument);

            EXPECT_FALSE(store.find(102).has_value());
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreTest, TracksMultipleOrdersForSameAccountAndAsset) {
            OrderReservationStore store;

            ASSERT_TRUE(store.create(101, 1, 10, 300));
            ASSERT_TRUE(store.create(102, 1, 10, 500));

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
            EXPECT_EQ(store.find(102), (OrderReservation{1, 10, 500, 500}));
        }

        TEST(OrderReservationStoreTest, TracksMultipleAccountsAndAssets) {
            OrderReservationStore store;

            ASSERT_TRUE(store.create(101, 1, 10, 300));
            ASSERT_TRUE(store.create(102, 2, 20, 500));
            ASSERT_TRUE(store.create(103, 1, 20, 700));

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
            EXPECT_EQ(store.find(102), (OrderReservation{2, 20, 500, 500}));
            EXPECT_EQ(store.find(103), (OrderReservation{1, 20, 700, 700}));
        }

        TEST(OrderReservationStoreTest, IdenticalSequencesProduceIdenticalResults) {
            OrderReservationStore first;
            OrderReservationStore second;

            EXPECT_EQ(first.create(102, 2, 20, 500),
                      second.create(102, 2, 20, 500));
            EXPECT_EQ(first.create(101, 1, 10, 300),
                      second.create(101, 1, 10, 300));
            EXPECT_EQ(first.create(102, 3, 30, 700),
                      second.create(102, 3, 30, 700));
            EXPECT_EQ(first.remove(101), second.remove(101));

            EXPECT_EQ(first.find(101), second.find(101));
            EXPECT_EQ(first.find(102), second.find(102));
            EXPECT_EQ(first.find(103), second.find(103));
        }

        TEST(OrderReservationStoreConsumeTest, PartialConsumeReducesRemainingOnly) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            store.consume(101, 120);

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 180}));
        }

        TEST(OrderReservationStoreConsumeTest, MultipleConsumesPreserveOriginalAmount) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            store.consume(101, 100);
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 200}));

            store.consume(101, 50);
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 150}));
        }

        TEST(OrderReservationStoreConsumeTest, ExactConsumeRetainsOwnershipRecordAtZero) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            store.consume(101, 300);

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 0}));
        }

        TEST(OrderReservationStoreConsumeTest, OverConsumeLeavesRecordUnchanged) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));
            store.consume(101, 120);
            const auto before = store.find(101);

            EXPECT_THROW(store.consume(101, 181), std::logic_error);
            EXPECT_EQ(store.find(101), before);
        }

        TEST(OrderReservationStoreConsumeTest, CannotConsumeAnAlreadyZeroRecord) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));
            store.consume(101, 300);
            const auto before = store.find(101);

            EXPECT_THROW(store.consume(101, 1), std::logic_error);
            EXPECT_EQ(store.find(101), before);
        }

        TEST(OrderReservationStoreConsumeTest, MissingOrderThrowsWithoutChangingStore) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            EXPECT_THROW(store.consume(102, 1), std::out_of_range);
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
            EXPECT_FALSE(store.find(102).has_value());
        }

        TEST(OrderReservationStoreConsumeTest, RejectsInvalidOrderAndAmount) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));

            EXPECT_THROW(store.consume(0, 1), std::invalid_argument);
            EXPECT_THROW(store.consume(101, 0), std::invalid_argument);
            EXPECT_THROW(store.consume(101, -1), std::invalid_argument);
            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 300}));
        }

        TEST(OrderReservationStoreConsumeTest, ConsumingOneOrderDoesNotAffectAnother) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));
            ASSERT_TRUE(store.create(102, 2, 20, 500));

            store.consume(101, 120);

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 180}));
            EXPECT_EQ(store.find(102), (OrderReservation{2, 20, 500, 500}));
        }

        TEST(OrderReservationStoreConsumeTest, SameAccountAssetOrdersRemainIndependent) {
            OrderReservationStore store;
            ASSERT_TRUE(store.create(101, 1, 10, 300));
            ASSERT_TRUE(store.create(102, 1, 10, 500));

            store.consume(101, 100);
            store.consume(102, 200);

            EXPECT_EQ(store.find(101), (OrderReservation{1, 10, 300, 200}));
            EXPECT_EQ(store.find(102), (OrderReservation{1, 10, 500, 300}));
        }

        TEST(OrderReservationStoreConsumeTest, IdenticalSequencesHaveIdenticalFailurePoints) {
            OrderReservationStore first;
            OrderReservationStore second;
            ASSERT_TRUE(first.create(101, 1, 10, 300));
            ASSERT_TRUE(second.create(101, 1, 10, 300));

            first.consume(101, 100);
            second.consume(101, 100);
            first.consume(101, 200);
            second.consume(101, 200);

            EXPECT_THROW(first.consume(101, 1), std::logic_error);
            EXPECT_THROW(second.consume(101, 1), std::logic_error);
            EXPECT_EQ(first.find(101), second.find(101));
        }
    }  // namespace
}  // namespace exchange
