#include "exchange/accounting/funding_coordinator.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr AccountId treasury_id = 1;
        constexpr AccountId user_a_id = 2;
        constexpr AccountId user_b_id = 3;
        constexpr AssetId asset_a = 10;
        constexpr AssetId asset_b = 20;

        void create_account(AccountStore& accounts, AccountId account_id) {
            ASSERT_TRUE(accounts.create_account(account_id));
        }

        void replay_funding_ledger(
            const Ledger& ledger,
            AccountStore& accounts) {
            LedgerSequence expected_sequence = 1;
            for (const LedgerEntry& entry : ledger.entries()) {
                if (entry.sequence != expected_sequence++) {
                    throw std::logic_error(
                        "Funding replay sequence is not contiguous");
                }
                if (!std::holds_alternative<FundingLedgerMetadata>(
                        entry.transaction.metadata)
                    || entry.transaction.postings.size() != 2) {
                    throw std::logic_error(
                        "Funding replay received a non-Funding entry");
                }

                const Posting& source = entry.transaction.postings[0];
                const Posting& destination =
                    entry.transaction.postings[1];
                if (accounts.transfer_available(
                        source.account_id,
                        destination.account_id,
                        source.asset_id,
                        -source.delta)
                    != AvailableTransferResult::Success) {
                    throw std::logic_error(
                        "committed Funding cannot be replayed");
                }
            }
        }

        TEST(FundingCoordinatorTest,
             RejectsZeroTreasuryButDefersExistenceToOperationTime) {
            AccountStore accounts;
            Ledger ledger;

            EXPECT_THROW(
                FundingCoordinator(0, accounts, ledger),
                std::invalid_argument);

            create_account(accounts, user_a_id);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);
            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_a_id, asset_a, 10)),
                std::out_of_range);
            EXPECT_FALSE(accounts.contains_account(treasury_id));
            EXPECT_FALSE(accounts.find_balance(user_a_id, asset_a).has_value());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(FundingCoordinatorTest,
             FundsMissingRecipientAssetWithCanonicalHistoryAndConservedSupply) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 1'000);
            ASSERT_EQ(
                accounts.reserve(treasury_id, asset_a, 200),
                ReserveResult::Success);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 300),
                FundingResult::Funded);

            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{500, 200}));
            EXPECT_EQ(
                accounts.find_balance(user_a_id, asset_a),
                (Balance{300, 0}));
            const Balance treasury =
                *accounts.find_balance(treasury_id, asset_a);
            const Balance recipient =
                *accounts.find_balance(user_a_id, asset_a);
            EXPECT_EQ(
                treasury.available + treasury.reserved
                    + recipient.available + recipient.reserved,
                1'000);

            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(
                ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_funding_ledger_transaction(
                        treasury_id,
                        user_a_id,
                        asset_a,
                        300)}));

            AccountStore replayed;
            create_account(replayed, treasury_id);
            create_account(replayed, user_a_id);
            replayed.fund(treasury_id, asset_a, 1'000);
            ASSERT_EQ(
                replayed.reserve(treasury_id, asset_a, 200),
                ReserveResult::Success);
            replay_funding_ledger(ledger, replayed);
            EXPECT_EQ(
                replayed.find_balance(treasury_id, asset_a),
                accounts.find_balance(treasury_id, asset_a));
            EXPECT_EQ(
                replayed.find_balance(user_a_id, asset_a),
                accounts.find_balance(user_a_id, asset_a));
        }

        TEST(FundingCoordinatorTest,
             ExistingRecipientAndOtherAssetStateRemainIndependent) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 1'000);
            accounts.fund(treasury_id, asset_b, 400);
            accounts.fund(user_a_id, asset_a, 500);
            accounts.fund(user_a_id, asset_b, 50);
            ASSERT_EQ(
                accounts.reserve(user_a_id, asset_a, 200),
                ReserveResult::Success);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 100),
                FundingResult::Funded);

            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{900, 0}));
            EXPECT_EQ(
                accounts.find_balance(user_a_id, asset_a),
                (Balance{400, 200}));
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_b),
                (Balance{400, 0}));
            EXPECT_EQ(
                accounts.find_balance(user_a_id, asset_b),
                (Balance{50, 0}));

            AccountStore replayed;
            create_account(replayed, treasury_id);
            create_account(replayed, user_a_id);
            replayed.fund(treasury_id, asset_a, 1'000);
            replayed.fund(treasury_id, asset_b, 400);
            replayed.fund(user_a_id, asset_a, 500);
            replayed.fund(user_a_id, asset_b, 50);
            ASSERT_EQ(
                replayed.reserve(user_a_id, asset_a, 200),
                ReserveResult::Success);
            replay_funding_ledger(ledger, replayed);
            for (const AccountId account_id :
                 {treasury_id, user_a_id}) {
                for (const AssetId asset_id : {asset_a, asset_b}) {
                    EXPECT_EQ(
                        replayed.find_balance(account_id, asset_id),
                        accounts.find_balance(account_id, asset_id));
                }
            }
        }

        TEST(FundingCoordinatorTest, ExactTreasuryBalanceCanBeTransferred) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 250);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 250),
                FundingResult::Funded);
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{0, 0}));
            EXPECT_EQ(
                accounts.find_balance(user_a_id, asset_a),
                (Balance{250, 0}));
        }

        TEST(FundingCoordinatorTest,
             InsufficientTreasuryFailuresDoNotMutateOrConsumeSequence) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            create_account(accounts, user_b_id);
            accounts.fund(treasury_id, asset_a, 100);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 40),
                FundingResult::Funded);
            const auto treasury_before =
                accounts.find_balance(treasury_id, asset_a);
            EXPECT_EQ(
                coordinator.fund(user_b_id, asset_a, 100),
                FundingResult::InsufficientTreasuryFunds);
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                treasury_before);
            EXPECT_FALSE(accounts.find_balance(user_b_id, asset_a).has_value());
            EXPECT_EQ(
                coordinator.fund(user_b_id, asset_b, 1),
                FundingResult::InsufficientTreasuryFunds);
            EXPECT_FALSE(accounts.find_balance(treasury_id, asset_b).has_value());
            EXPECT_FALSE(accounts.find_balance(user_b_id, asset_b).has_value());

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 10),
                FundingResult::Funded);
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
        }

        TEST(FundingCoordinatorTest,
             MissingRecipientPropagatesAndReleasesPreparedBatch) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 100);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_b_id, asset_a, 10)),
                std::out_of_range);
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{100, 0}));
            EXPECT_TRUE(ledger.entries().empty());

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 10),
                FundingResult::Funded);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(FundingCoordinatorTest,
             RejectsInvalidInputsBeforeAnyFinancialMutation) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 100);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_THROW(
                static_cast<void>(coordinator.fund(0, asset_a, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(coordinator.fund(user_a_id, 0, 1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_a_id, asset_a, 0)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_a_id, asset_a, -1)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(treasury_id, asset_a, 1)),
                std::invalid_argument);

            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{100, 0}));
            EXPECT_FALSE(accounts.find_balance(user_a_id, asset_a).has_value());
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(FundingCoordinatorTest,
             DestinationOverflowAbandonsBatchAndPreservesNextSequence) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 10);
            accounts.fund(
                user_a_id,
                asset_a,
                std::numeric_limits<Amount>::max() - 5);
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_a_id, asset_a, 6)),
                std::overflow_error);
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{10, 0}));
            EXPECT_EQ(
                accounts.find_balance(user_a_id, asset_a),
                (Balance{std::numeric_limits<Amount>::max() - 5, 0}));
            EXPECT_TRUE(ledger.entries().empty());

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 5),
                FundingResult::Funded);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(FundingCoordinatorTest,
             LedgerPreparationFailurePrecedesAccountStoreMutation) {
            AccountStore accounts;
            Ledger ledger;
            create_account(accounts, treasury_id);
            create_account(accounts, user_a_id);
            accounts.fund(treasury_id, asset_a, 100);
            auto active_batch = ledger.prepare_batch({
                make_funding_ledger_transaction(
                    treasury_id,
                    user_a_id,
                    asset_a,
                    1),
            });
            FundingCoordinator coordinator(treasury_id, accounts, ledger);

            EXPECT_THROW(
                static_cast<void>(
                    coordinator.fund(user_a_id, asset_a, 10)),
                std::logic_error);
            EXPECT_EQ(
                accounts.find_balance(treasury_id, asset_a),
                (Balance{100, 0}));
            EXPECT_FALSE(accounts.find_balance(user_a_id, asset_a).has_value());
            EXPECT_TRUE(ledger.entries().empty());

            static_cast<void>(active_batch);
        }

        TEST(FundingCoordinatorTest,
             MultipleFundingOperationsAreDeterministicAndReplayEquivalent) {
            AccountStore runtime;
            AccountStore replayed;
            Ledger ledger;
            for (AccountStore* accounts : {&runtime, &replayed}) {
                create_account(*accounts, treasury_id);
                create_account(*accounts, user_a_id);
                create_account(*accounts, user_b_id);
                accounts->fund(treasury_id, asset_a, 1'000);
                accounts->fund(treasury_id, asset_b, 500);
            }
            FundingCoordinator coordinator(treasury_id, runtime, ledger);

            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 100),
                FundingResult::Funded);
            EXPECT_EQ(
                coordinator.fund(user_b_id, asset_a, 200),
                FundingResult::Funded);
            EXPECT_EQ(
                coordinator.fund(user_a_id, asset_a, 50),
                FundingResult::Funded);
            EXPECT_EQ(
                coordinator.fund(user_b_id, asset_b, 25),
                FundingResult::Funded);

            ASSERT_EQ(ledger.entries().size(), 4U);
            const LedgerTransaction expected[] = {
                make_funding_ledger_transaction(
                    treasury_id, user_a_id, asset_a, 100),
                make_funding_ledger_transaction(
                    treasury_id, user_b_id, asset_a, 200),
                make_funding_ledger_transaction(
                    treasury_id, user_a_id, asset_a, 50),
                make_funding_ledger_transaction(
                    treasury_id, user_b_id, asset_b, 25),
            };
            for (std::size_t index = 0; index < ledger.entries().size();
                 ++index) {
                EXPECT_EQ(ledger.entries()[index].sequence, index + 1);
                EXPECT_EQ(ledger.entries()[index].transaction, expected[index]);
            }

            replay_funding_ledger(ledger, replayed);
            for (const AccountId account_id :
                 {treasury_id, user_a_id, user_b_id}) {
                for (const AssetId asset_id : {asset_a, asset_b}) {
                    EXPECT_EQ(
                        replayed.find_balance(account_id, asset_id),
                        runtime.find_balance(account_id, asset_id));
                }
            }

            AccountStore deterministic_accounts;
            Ledger deterministic_ledger;
            create_account(deterministic_accounts, treasury_id);
            create_account(deterministic_accounts, user_a_id);
            create_account(deterministic_accounts, user_b_id);
            deterministic_accounts.fund(treasury_id, asset_a, 1'000);
            deterministic_accounts.fund(treasury_id, asset_b, 500);
            FundingCoordinator deterministic_coordinator(
                treasury_id,
                deterministic_accounts,
                deterministic_ledger);
            EXPECT_EQ(
                deterministic_coordinator.fund(user_a_id, asset_a, 100),
                FundingResult::Funded);
            EXPECT_EQ(
                deterministic_coordinator.fund(user_b_id, asset_a, 200),
                FundingResult::Funded);
            EXPECT_EQ(
                deterministic_coordinator.fund(user_a_id, asset_a, 50),
                FundingResult::Funded);
            EXPECT_EQ(
                deterministic_coordinator.fund(user_b_id, asset_b, 25),
                FundingResult::Funded);
            EXPECT_EQ(deterministic_ledger.entries(), ledger.entries());
        }

        static_assert(!std::is_copy_constructible_v<FundingCoordinator>);
        static_assert(!std::is_move_constructible_v<FundingCoordinator>);
    }  // namespace
}  // namespace exchange
