#include "exchange/account_store.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        TEST(AccountStoreTest, CreatesAndFindsAccounts) {
            AccountStore store;

            EXPECT_FALSE(store.contains_account(1));
            EXPECT_TRUE(store.create_account(1));
            EXPECT_TRUE(store.contains_account(1));
            EXPECT_FALSE(store.create_account(1));
            EXPECT_FALSE(store.find_balance(1, 1).has_value());
            EXPECT_FALSE(store.find_balance(2, 1).has_value());
        }

        TEST(AccountStoreTest, FundProvidesPositiveBootstrapTestBalanceSnapshot) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));

            store.fund(1, 10, 1'000);
            auto snapshot = store.find_balance(1, 10);

            ASSERT_TRUE(snapshot.has_value());
            EXPECT_EQ(*snapshot, (Balance{1'000, 0}));

            snapshot->available = 0;
            EXPECT_EQ(store.find_balance(1, 10), (Balance{1'000, 0}));

            store.fund(1, 10, 250);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{1'250, 0}));
        }

        TEST(AccountStoreTest, TracksMultipleAssetsIndependently) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            ASSERT_TRUE(store.create_account(2));

            store.fund(1, 10, 500);
            store.fund(1, 20, 750);
            store.fund(2, 10, 900);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{500, 0}));
            EXPECT_EQ(store.find_balance(1, 20), (Balance{750, 0}));
            EXPECT_EQ(store.find_balance(2, 10), (Balance{900, 0}));
            EXPECT_FALSE(store.find_balance(2, 20).has_value());
        }

        TEST(AccountStoreInvariantTest, RejectsInvalidAccountIdWithoutCreatingState) {
            AccountStore store;

            EXPECT_THROW(static_cast<void>(store.create_account(0)),
                         std::invalid_argument);
            EXPECT_FALSE(store.contains_account(0));
            EXPECT_THROW(static_cast<void>(store.find_balance(0, 1)),
                         std::invalid_argument);
        }

        TEST(AccountStoreInvariantTest, RejectsInvalidAssetIdConsistently) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));

            EXPECT_THROW(store.fund(1, 0, 100), std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.find_balance(1, 0)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.find_balance(2, 0)),
                         std::invalid_argument);
            EXPECT_FALSE(store.find_balance(1, 1).has_value());
        }

        TEST(AccountStoreInvariantTest, MissingAccountAndAssetRemainAbsent) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 500);

            EXPECT_FALSE(store.find_balance(2, 10).has_value());
            EXPECT_THROW(store.fund(2, 10, 100), std::out_of_range);
            EXPECT_FALSE(store.contains_account(2));

            EXPECT_FALSE(store.find_balance(1, 20).has_value());
            EXPECT_FALSE(store.find_balance(1, 20).has_value());
            EXPECT_EQ(store.find_balance(1, 10), (Balance{500, 0}));
        }

        TEST(AccountStoreInvariantTest, RejectsNonPositiveFundingWithoutCreatingAsset) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));

            EXPECT_THROW(store.fund(1, 10, 0), std::invalid_argument);
            EXPECT_FALSE(store.find_balance(1, 10).has_value());

            EXPECT_THROW(store.fund(1, 10, -1), std::invalid_argument);
            EXPECT_FALSE(store.find_balance(1, 10).has_value());
        }

        TEST(AccountStoreInvariantTest, OverflowLeavesBalanceUnchanged) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            constexpr Amount maximum = std::numeric_limits<Amount>::max();

            store.fund(1, 10, maximum);
            ASSERT_EQ(store.find_balance(1, 10), (Balance{maximum, 0}));

            EXPECT_THROW(store.fund(1, 10, 1), std::overflow_error);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{maximum, 0}));
        }

        TEST(AccountStoreInvariantTest, FundNeverChangesReservedBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));

            store.fund(1, 10, 100);
            store.fund(1, 10, 200);
            store.fund(1, 20, 300);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{300, 0}));
            EXPECT_EQ(store.find_balance(1, 20), (Balance{300, 0}));
        }

        TEST(AccountStoreInvariantTest, DuplicateCreationPreservesExistingBalances) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 400);

            EXPECT_FALSE(store.create_account(1));
            EXPECT_EQ(store.find_balance(1, 10), (Balance{400, 0}));
        }

        TEST(AccountStoreDeterminismTest, SameSequenceProducesSameObservableState) {
            AccountStore first;
            AccountStore second;

            const auto apply_sequence = [](AccountStore& store) {
                EXPECT_TRUE(store.create_account(20));
                EXPECT_TRUE(store.create_account(10));
                store.fund(20, 200, 700);
                store.fund(10, 100, 300);
                store.fund(20, 100, 500);
                store.fund(10, 100, 200);
            };

            apply_sequence(first);
            apply_sequence(second);

            for (const AccountId account_id : {AccountId{10}, AccountId{20}}) {
                EXPECT_EQ(first.contains_account(account_id),
                          second.contains_account(account_id));
                for (const AssetId asset_id : {AssetId{100}, AssetId{200}}) {
                    EXPECT_EQ(first.find_balance(account_id, asset_id),
                              second.find_balance(account_id, asset_id));
                }
            }
        }

        TEST(AccountStoreReserveTest, PartialReserveMovesAvailableToReserved) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 1'000);

            EXPECT_EQ(store.reserve(1, 10, 300), ReserveResult::Success);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{700, 300}));
        }

        TEST(AccountStoreReserveTest, CanReserveExactAvailableBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 500);

            EXPECT_EQ(store.reserve(1, 10, 500), ReserveResult::Success);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{0, 500}));
        }

        TEST(AccountStoreReserveTest, RejectionLeavesStateUnchangedAndCreatesNoAsset) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);

            const auto before = store.find_balance(1, 10);
            EXPECT_EQ(store.reserve(1, 10, 101),
                      ReserveResult::InsufficientFunds);
            EXPECT_EQ(store.find_balance(1, 10), before);

            EXPECT_EQ(store.reserve(1, 20, 1),
                      ReserveResult::InsufficientFunds);
            EXPECT_FALSE(store.find_balance(1, 20).has_value());
        }

        TEST(AccountStoreReserveTest, ReservedOverflowLeavesStateUnchanged) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            store.fund(1, 10, maximum);
            ASSERT_EQ(store.reserve(1, 10, maximum), ReserveResult::Success);
            store.fund(1, 10, 1);
            const auto before = store.find_balance(1, 10);

            EXPECT_THROW(static_cast<void>(store.reserve(1, 10, 1)),
                         std::overflow_error);
            EXPECT_EQ(store.find_balance(1, 10), before);
        }

        TEST(AccountStoreReserveTest, RejectsInvalidInputAndMissingAccount) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);

            EXPECT_THROW(static_cast<void>(store.reserve(0, 10, 1)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.reserve(1, 0, 1)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.reserve(1, 10, 0)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.reserve(1, 10, -1)),
                         std::invalid_argument);
            EXPECT_THROW(static_cast<void>(store.reserve(2, 10, 1)),
                         std::out_of_range);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{100, 0}));
            EXPECT_FALSE(store.contains_account(2));
        }

        TEST(AccountStoreReleaseTest, PartialReleaseMovesReservedToAvailable) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 1'000);
            ASSERT_EQ(store.reserve(1, 10, 400), ReserveResult::Success);

            store.release(1, 10, 150);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{750, 250}));
        }

        TEST(AccountStoreReleaseTest, CanReleaseExactReservedBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 500);
            ASSERT_EQ(store.reserve(1, 10, 500), ReserveResult::Success);

            store.release(1, 10, 500);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{500, 0}));
        }

        TEST(AccountStoreReleaseTest, InvalidReservationStateDoesNotMutateBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);
            ASSERT_EQ(store.reserve(1, 10, 40), ReserveResult::Success);
            const auto before = store.find_balance(1, 10);

            EXPECT_THROW(store.release(1, 10, 41), std::logic_error);
            EXPECT_EQ(store.find_balance(1, 10), before);

            EXPECT_THROW(store.release(1, 20, 1), std::logic_error);
            EXPECT_FALSE(store.find_balance(1, 20).has_value());
            EXPECT_EQ(store.find_balance(1, 10), before);
        }

        TEST(AccountStoreReleaseTest, AvailableOverflowLeavesStateUnchanged) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            store.fund(1, 10, maximum);
            ASSERT_EQ(store.reserve(1, 10, 1), ReserveResult::Success);
            store.fund(1, 10, 1);
            const auto before = store.find_balance(1, 10);

            EXPECT_THROW(store.release(1, 10, 1), std::overflow_error);
            EXPECT_EQ(store.find_balance(1, 10), before);
        }

        TEST(AccountStoreReleaseTest, RejectsInvalidInputAndMissingAccount) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);

            EXPECT_THROW(store.release(0, 10, 1), std::invalid_argument);
            EXPECT_THROW(store.release(1, 0, 1), std::invalid_argument);
            EXPECT_THROW(store.release(1, 10, 0), std::invalid_argument);
            EXPECT_THROW(store.release(1, 10, -1), std::invalid_argument);
            EXPECT_THROW(store.release(2, 10, 1), std::out_of_range);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{100, 0}));
            EXPECT_FALSE(store.contains_account(2));
        }

        TEST(AccountStoreConsumeTest, PartialConsumeReducesReservedOnly) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 1'000);
            ASSERT_EQ(store.reserve(1, 10, 300), ReserveResult::Success);

            store.consume_reserved(1, 10, 120);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{700, 180}));
        }

        TEST(AccountStoreConsumeTest, CanConsumeExactReservedBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 500);
            ASSERT_EQ(store.reserve(1, 10, 500), ReserveResult::Success);

            store.consume_reserved(1, 10, 500);

            EXPECT_EQ(store.find_balance(1, 10), (Balance{0, 0}));
        }

        TEST(AccountStoreConsumeTest, InvalidReservationStateDoesNotMutateBalance) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);
            ASSERT_EQ(store.reserve(1, 10, 40), ReserveResult::Success);
            const auto before = store.find_balance(1, 10);

            EXPECT_THROW(store.consume_reserved(1, 10, 41), std::logic_error);
            EXPECT_EQ(store.find_balance(1, 10), before);

            EXPECT_THROW(store.consume_reserved(1, 20, 1), std::logic_error);
            EXPECT_FALSE(store.find_balance(1, 20).has_value());
            EXPECT_EQ(store.find_balance(1, 10), before);
        }

        TEST(AccountStoreConsumeTest, RejectsInvalidInputAndMissingAccount) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 100);

            EXPECT_THROW(store.consume_reserved(0, 10, 1),
                         std::invalid_argument);
            EXPECT_THROW(store.consume_reserved(1, 0, 1),
                         std::invalid_argument);
            EXPECT_THROW(store.consume_reserved(1, 10, 0),
                         std::invalid_argument);
            EXPECT_THROW(store.consume_reserved(1, 10, -1),
                         std::invalid_argument);
            EXPECT_THROW(store.consume_reserved(2, 10, 1),
                         std::out_of_range);
            EXPECT_EQ(store.find_balance(1, 10), (Balance{100, 0}));
            EXPECT_FALSE(store.contains_account(2));
        }

        TEST(AccountStoreReservationTest, MultipleAssetsRemainIndependentAndNonNegative) {
            AccountStore store;
            ASSERT_TRUE(store.create_account(1));
            store.fund(1, 10, 1'000);
            store.fund(1, 20, 500);

            ASSERT_EQ(store.reserve(1, 10, 300), ReserveResult::Success);
            ASSERT_EQ(store.reserve(1, 20, 200), ReserveResult::Success);
            store.release(1, 10, 100);
            store.consume_reserved(1, 20, 50);

            const auto first = store.find_balance(1, 10);
            const auto second = store.find_balance(1, 20);
            ASSERT_TRUE(first.has_value());
            ASSERT_TRUE(second.has_value());
            EXPECT_EQ(*first, (Balance{800, 200}));
            EXPECT_EQ(*second, (Balance{300, 150}));
            EXPECT_GE(first->available, 0);
            EXPECT_GE(first->reserved, 0);
            EXPECT_GE(second->available, 0);
            EXPECT_GE(second->reserved, 0);
        }

        TEST(AccountStoreReservationTest, IdenticalSequencesHaveIdenticalResultsAndFailures) {
            AccountStore first;
            AccountStore second;
            ASSERT_TRUE(first.create_account(1));
            ASSERT_TRUE(second.create_account(1));
            first.fund(1, 10, 1'000);
            second.fund(1, 10, 1'000);

            EXPECT_EQ(first.reserve(1, 10, 400), second.reserve(1, 10, 400));
            EXPECT_EQ(first.reserve(1, 10, 700), second.reserve(1, 10, 700));
            first.release(1, 10, 100);
            second.release(1, 10, 100);
            first.consume_reserved(1, 10, 150);
            second.consume_reserved(1, 10, 150);

            EXPECT_THROW(first.consume_reserved(1, 10, 151), std::logic_error);
            EXPECT_THROW(second.consume_reserved(1, 10, 151), std::logic_error);
            EXPECT_EQ(first.find_balance(1, 10), second.find_balance(1, 10));
        }
    }  // namespace
}  // namespace exchange
