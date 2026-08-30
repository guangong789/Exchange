#include "exchange/account_store.hpp"
#include "exchange/execution_coordinator.hpp"
#include "exchange/ledger.hpp"

#include <array>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        LedgerTransaction reserve_transaction(
            OrderId order_id = 101,
            Amount amount = 300) {
            return LedgerTransaction{
                ReserveLedgerMetadata{order_id},
                {
                    Posting{
                        1,
                        10,
                        BalanceBucket::Available,
                        -amount},
                    Posting{
                        1,
                        10,
                        BalanceBucket::Reserved,
                        amount},
                },
            };
        }

        LedgerTransaction release_transaction(
            OrderId order_id = 101,
            Amount amount = 300) {
            return LedgerTransaction{
                ReleaseLedgerMetadata{order_id},
                {
                    Posting{
                        1,
                        10,
                        BalanceBucket::Reserved,
                        -amount},
                    Posting{
                        1,
                        10,
                        BalanceBucket::Available,
                        amount},
                },
            };
        }

        LedgerTransaction trade_transaction() {
            return LedgerTransaction{
                TradeLedgerMetadata{Trade{101, 202, 95, 3, 11}},
                {
                    Posting{
                        1,
                        2,
                        BalanceBucket::Reserved,
                        -285},
                    Posting{
                        2,
                        1,
                        BalanceBucket::Reserved,
                        -3'000},
                    Posting{
                        1,
                        1,
                        BalanceBucket::Available,
                        3'000},
                    Posting{
                        2,
                        2,
                        BalanceBucket::Available,
                        285},
                },
            };
        }

        LedgerTransaction funding_transaction(
            AccountId source_account_id = 10,
            AccountId destination_account_id = 20,
            AssetId asset_id = 7,
            Amount amount = 500) {
            return LedgerTransaction{
                FundingLedgerMetadata{
                    source_account_id,
                    destination_account_id},
                {
                    Posting{
                        source_account_id,
                        asset_id,
                        BalanceBucket::Available,
                        -amount},
                    Posting{
                        destination_account_id,
                        asset_id,
                        BalanceBucket::Available,
                        amount},
                },
            };
        }

        Order limit_order(
            OrderId id,
            Side side,
            Price price,
            Quantity quantity,
            Timestamp timestamp = 0) {
            return Order{id, side, OrderType::Limit, price, quantity, timestamp};
        }

        void replay_ledger_into_account_store(
            const Ledger& ledger,
            AccountStore& accounts) {
            LedgerSequence expected_sequence = 1;
            for (const LedgerEntry& entry : ledger.entries()) {
                if (entry.sequence != expected_sequence) {
                    throw std::logic_error(
                        "Ledger replay sequence is not contiguous");
                }
                ++expected_sequence;

                const LedgerTransaction& transaction = entry.transaction;
                const auto& postings = transaction.postings;
                if (std::holds_alternative<ReserveLedgerMetadata>(
                        transaction.metadata)) {
                    const Posting& available = postings[0];
                    const Amount amount = -available.delta;
                    if (accounts.reserve(
                            available.account_id,
                            available.asset_id,
                            amount)
                        != ReserveResult::Success) {
                        throw std::logic_error(
                            "Ledger Reserve cannot be replayed from genesis");
                    }
                    continue;
                }

                if (std::holds_alternative<ReleaseLedgerMetadata>(
                        transaction.metadata)) {
                    const Posting& reserved = postings[0];
                    accounts.release(
                        reserved.account_id,
                        reserved.asset_id,
                        -reserved.delta);
                    continue;
                }

                const Posting& buyer_quote = postings[0];
                const Posting& seller_base = postings[1];
                const Posting& buyer_base = postings[2];
                const Posting& seller_quote = postings[3];
                accounts.consume_reserved(
                    buyer_quote.account_id,
                    buyer_quote.asset_id,
                    -buyer_quote.delta);
                accounts.consume_reserved(
                    seller_base.account_id,
                    seller_base.asset_id,
                    -seller_base.delta);
                accounts.credit_available(
                    buyer_base.account_id,
                    buyer_base.asset_id,
                    buyer_base.delta);
                accounts.credit_available(
                    seller_quote.account_id,
                    seller_quote.asset_id,
                    seller_quote.delta);
            }
        }

        void expect_balances_equal(
            const AccountStore& actual,
            const AccountStore& replayed,
            std::initializer_list<AccountId> account_ids,
            std::initializer_list<AssetId> asset_ids) {
            for (const AccountId account_id : account_ids) {
                for (const AssetId asset_id : asset_ids) {
                    EXPECT_EQ(
                        actual.find_balance(account_id, asset_id),
                        replayed.find_balance(account_id, asset_id))
                        << "account=" << account_id
                        << " asset=" << asset_id;
                }
            }
        }

        struct CoordinatorHarness {
            explicit CoordinatorHarness(InstrumentContext context)
                : instrument(context),
                  coordinator{
                      instrument,
                      accounts,
                      reservations,
                      matching_engine,
                      events,
                      ledger} {}

            const InstrumentContext instrument;
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator coordinator;
        };

        TEST(LedgerTest, AppendsCanonicalReserveAndPreservesValueOrder) {
            Ledger ledger;
            const LedgerTransaction transaction = reserve_transaction();

            ledger.append(transaction);

            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0], (LedgerEntry{1, transaction}));
            ASSERT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                ledger.entries()[0].transaction.metadata));
            EXPECT_EQ(
                ledger.entries()[0].transaction.postings,
                transaction.postings);
        }

        TEST(LedgerTest, AppendsCanonicalRelease) {
            Ledger ledger;
            const LedgerTransaction transaction = release_transaction();

            ledger.append(transaction);

            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].transaction, transaction);
        }

        TEST(LedgerTest, AppendsCanonicalTradeBalancedIndependentlyPerAsset) {
            Ledger ledger;
            const LedgerTransaction transaction = trade_transaction();

            ledger.append(transaction);

            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].transaction, transaction);
            EXPECT_EQ(
                ledger.entries()[0].transaction.postings[0].delta
                    + ledger.entries()[0].transaction.postings[3].delta,
                0);
            EXPECT_EQ(
                ledger.entries()[0].transaction.postings[1].delta
                    + ledger.entries()[0].transaction.postings[2].delta,
                0);
        }

        TEST(LedgerTest, RejectsEmptyTransaction) {
            Ledger ledger;

            EXPECT_THROW(
                ledger.append(LedgerTransaction{
                    ReserveLedgerMetadata{101},
                    {}}),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsInvalidPostingFields) {
            const auto expect_invalid = [](Posting invalid_posting) {
                Ledger ledger;
                LedgerTransaction transaction = reserve_transaction();
                transaction.postings[0] = invalid_posting;
                EXPECT_THROW(
                    ledger.append(std::move(transaction)),
                    std::invalid_argument);
                EXPECT_TRUE(ledger.entries().empty());
            };

            expect_invalid(Posting{
                0,
                10,
                BalanceBucket::Available,
                -300});
            expect_invalid(Posting{
                1,
                0,
                BalanceBucket::Available,
                -300});
            expect_invalid(Posting{
                1,
                10,
                static_cast<BalanceBucket>(255),
                -300});
            expect_invalid(Posting{
                1,
                10,
                BalanceBucket::Available,
                0});
            expect_invalid(Posting{
                1,
                10,
                BalanceBucket::Available,
                std::numeric_limits<Amount>::min()});
        }

        TEST(LedgerTest, RejectsInvalidReserveAndReleaseMetadata) {
            Ledger ledger;

            EXPECT_THROW(
                ledger.append(reserve_transaction(0)),
                std::invalid_argument);
            EXPECT_THROW(
                ledger.append(release_transaction(0)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsInvalidTradeMetadata) {
            const auto expect_invalid = [](Trade trade) {
                Ledger ledger;
                LedgerTransaction transaction = trade_transaction();
                transaction.metadata = TradeLedgerMetadata{trade};
                EXPECT_THROW(
                    ledger.append(std::move(transaction)),
                    std::invalid_argument);
                EXPECT_TRUE(ledger.entries().empty());
            };

            expect_invalid(Trade{0, 202, 95, 3, 11});
            expect_invalid(Trade{101, 0, 95, 3, 11});
            expect_invalid(Trade{101, 202, 0, 3, 11});
            expect_invalid(Trade{101, 202, 95, 0, 11});
            expect_invalid(Trade{101, 202, -1, 3, 11});
            expect_invalid(Trade{101, 202, 95, -1, 11});
        }

        TEST(LedgerTest, RejectsInvalidReserveShapes) {
            const auto expect_invalid = [](LedgerTransaction transaction) {
                Ledger ledger;
                EXPECT_THROW(
                    ledger.append(std::move(transaction)),
                    std::invalid_argument);
                EXPECT_TRUE(ledger.entries().empty());
            };

            LedgerTransaction unbalanced = reserve_transaction();
            unbalanced.postings[1].delta = 299;
            expect_invalid(std::move(unbalanced));

            LedgerTransaction wrong_order = reserve_transaction();
            std::swap(wrong_order.postings[0], wrong_order.postings[1]);
            expect_invalid(std::move(wrong_order));

            LedgerTransaction different_accounts = reserve_transaction();
            different_accounts.postings[1].account_id = 2;
            expect_invalid(std::move(different_accounts));

            LedgerTransaction different_assets = reserve_transaction();
            different_assets.postings[1].asset_id = 20;
            expect_invalid(std::move(different_assets));

            LedgerTransaction extra_posting = reserve_transaction();
            extra_posting.postings.push_back(Posting{
                1,
                10,
                BalanceBucket::Available,
                1});
            expect_invalid(std::move(extra_posting));
        }

        TEST(LedgerTest, RejectsInvalidReleaseCanonicalOrder) {
            Ledger ledger;
            LedgerTransaction transaction = release_transaction();
            std::swap(transaction.postings[0], transaction.postings[1]);

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeBaseImbalance) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[2].delta = 2'999;

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeQuoteImbalance) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[3].delta = 284;

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeWithSameBaseAndQuoteAsset) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[1].asset_id = 2;
            transaction.postings[2].asset_id = 2;

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeWithWrongBucketKind) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[2].bucket = BalanceBucket::Reserved;

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeWithWrongCanonicalOrder) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            std::swap(transaction.postings[1], transaction.postings[2]);

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, RejectsTradeWithExtraPosting) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings.push_back(Posting{
                3,
                3,
                BalanceBucket::Available,
                1});

            EXPECT_THROW(
                ledger.append(std::move(transaction)),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerTest, SupportsDifferentBalancedMagnitudesForDifferentAssets) {
            Ledger ledger;
            const LedgerTransaction transaction = trade_transaction();

            EXPECT_NO_THROW(ledger.append(transaction));
            EXPECT_EQ(ledger.entries()[0].transaction, transaction);
        }

        TEST(LedgerTest, ValidatesMaximumPostingMagnitudesWithoutNarrowAccumulation) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[0].delta = -maximum;
            transaction.postings[1].delta = -maximum;
            transaction.postings[2].delta = maximum;
            transaction.postings[3].delta = maximum;

            EXPECT_NO_THROW(ledger.append(std::move(transaction)));
            ASSERT_EQ(ledger.entries().size(), 1U);
        }

        TEST(LedgerTest, FailedAppendDoesNotConsumeSequence) {
            Ledger ledger;
            ledger.append(reserve_transaction(101));
            ledger.append(trade_transaction());
            ledger.append(release_transaction(102));

            LedgerTransaction invalid = reserve_transaction(103);
            invalid.postings[1].delta = 299;
            EXPECT_THROW(
                ledger.append(std::move(invalid)),
                std::invalid_argument);

            ledger.append(reserve_transaction(104));

            ASSERT_EQ(ledger.entries().size(), 4U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
            EXPECT_EQ(ledger.entries()[2].sequence, 3U);
            EXPECT_EQ(ledger.entries()[3].sequence, 4U);
        }

        TEST(LedgerTest, AppendBatchPreservesInputAndSequenceOrder) {
            Ledger ledger;
            const LedgerTransaction reserve = reserve_transaction(101);
            const LedgerTransaction trade = trade_transaction();
            const LedgerTransaction release = release_transaction(101);

            ledger.append_batch({reserve, trade, release});

            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_EQ(ledger.entries()[0], (LedgerEntry{1, reserve}));
            EXPECT_EQ(ledger.entries()[1], (LedgerEntry{2, trade}));
            EXPECT_EQ(ledger.entries()[2], (LedgerEntry{3, release}));
        }

        TEST(LedgerTest, InvalidBatchIsAllOrNoneAndPreservesSequence) {
            Ledger ledger;
            ledger.append(reserve_transaction(100));
            const auto history_before = ledger.entries();

            LedgerTransaction invalid_trade = trade_transaction();
            invalid_trade.postings[3].delta = 284;
            EXPECT_THROW(
                ledger.append_batch({
                    reserve_transaction(101),
                    invalid_trade,
                    release_transaction(101),
                }),
                std::invalid_argument);

            EXPECT_EQ(ledger.entries(), history_before);
            ledger.append(release_transaction(100));
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
        }

        TEST(LedgerTest, EmptyBatchIsNoOp) {
            Ledger ledger;
            ledger.append(reserve_transaction());
            const auto history_before = ledger.entries();

            EXPECT_NO_THROW(ledger.append_batch({}));

            EXPECT_EQ(ledger.entries(), history_before);
            ledger.append(release_transaction());
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
        }

        TEST(LedgerPreparedBatchTest, PrepareKeepsEntriesAndSequenceUncommitted) {
            Ledger ledger;
            ledger.append(reserve_transaction(100));
            const auto history_before = ledger.entries();
            const LedgerTransaction reserve = reserve_transaction(101);
            const LedgerTransaction release = release_transaction(101);

            auto prepared = ledger.prepare_batch({reserve, release});

            EXPECT_EQ(ledger.entries(), history_before);

            prepared.commit();

            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_EQ(ledger.entries()[0], history_before[0]);
            EXPECT_EQ(ledger.entries()[1], (LedgerEntry{2, reserve}));
            EXPECT_EQ(ledger.entries()[2], (LedgerEntry{3, release}));
        }

        TEST(LedgerPreparedBatchTest, AbandonLeavesHistoryAndNextSequenceUnchanged) {
            Ledger ledger;
            ledger.append(reserve_transaction(100));
            const auto history_before = ledger.entries();

            {
                auto prepared = ledger.prepare_batch({
                    reserve_transaction(101),
                    release_transaction(101),
                });
                EXPECT_EQ(ledger.entries(), history_before);
            }

            EXPECT_EQ(ledger.entries(), history_before);
            auto next = ledger.prepare_batch({release_transaction(100)});
            next.commit();
            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
        }

        TEST(LedgerPreparedBatchTest, CommitIsOneShotAndRepeatedCallIsNoOp) {
            Ledger ledger;
            auto prepared = ledger.prepare_batch({reserve_transaction(101)});

            prepared.commit();
            const auto history_after_commit = ledger.entries();
            prepared.commit();

            EXPECT_EQ(ledger.entries(), history_after_commit);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(LedgerPreparedBatchTest, MoveTransfersBatchAndLeavesSourceInert) {
            static_assert(std::is_nothrow_move_constructible_v<
                          Ledger::PreparedBatch>);
            static_assert(!std::is_copy_constructible_v<
                          Ledger::PreparedBatch>);
            static_assert(!std::is_move_assignable_v<
                          Ledger::PreparedBatch>);

            Ledger ledger;
            auto source = ledger.prepare_batch({reserve_transaction(101)});
            auto destination = std::move(source);

            source.commit();
            EXPECT_TRUE(ledger.entries().empty());

            destination.commit();
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(LedgerPreparedBatchTest, InvalidPrepareDoesNotLeakActiveStateOrSequence) {
            Ledger ledger;
            LedgerTransaction invalid = reserve_transaction(101);
            invalid.postings[1].delta = 299;

            EXPECT_THROW(
                static_cast<void>(ledger.prepare_batch({invalid})),
                std::invalid_argument);
            EXPECT_TRUE(ledger.entries().empty());

            auto prepared = ledger.prepare_batch({reserve_transaction(102)});
            prepared.commit();
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(LedgerPreparedBatchTest, RejectsSecondActiveBatchAndReleasesLockOnCommit) {
            Ledger ledger;
            auto first = ledger.prepare_batch({reserve_transaction(101)});

            EXPECT_THROW(
                static_cast<void>(ledger.prepare_batch({
                    reserve_transaction(102)})),
                std::logic_error);
            EXPECT_THROW(
                ledger.append(reserve_transaction(102)),
                std::logic_error);
            EXPECT_THROW(
                ledger.append_batch({reserve_transaction(102)}),
                std::logic_error);
            EXPECT_TRUE(ledger.entries().empty());

            first.commit();
            auto second = ledger.prepare_batch({reserve_transaction(102)});
            second.commit();

            ASSERT_EQ(ledger.entries().size(), 2U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
            EXPECT_EQ(ledger.entries()[1].sequence, 2U);
        }

        TEST(LedgerPreparedBatchTest, EmptyPreparedBatchIsInertAndNeedsNoLock) {
            Ledger ledger;
            auto active = ledger.prepare_batch({reserve_transaction(101)});
            auto empty = ledger.prepare_batch({});

            empty.commit();
            empty.commit();
            EXPECT_TRUE(ledger.entries().empty());

            active.commit();
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0].sequence, 1U);
        }

        TEST(LedgerTest, AllowsSameAccountTradeWhileBalancingEachAsset) {
            Ledger ledger;
            LedgerTransaction transaction = trade_transaction();
            transaction.postings[1].account_id = 1;
            transaction.postings[3].account_id = 1;

            EXPECT_NO_THROW(ledger.append(std::move(transaction)));
            EXPECT_EQ(ledger.entries().size(), 1U);
        }

        TEST(LedgerBuilderTest, BuildsCanonicalReserveAcceptedByLedger) {
            const LedgerTransaction transaction =
                make_reserve_ledger_transaction(101, 1, 10, 300);
            const LedgerTransaction expected{
                ReserveLedgerMetadata{101},
                {
                    Posting{
                        1,
                        10,
                        BalanceBucket::Available,
                        -300},
                    Posting{
                        1,
                        10,
                        BalanceBucket::Reserved,
                        300},
                },
            };

            EXPECT_EQ(transaction, expected);
            Ledger ledger;
            EXPECT_NO_THROW(ledger.append(transaction));
            EXPECT_EQ(ledger.entries()[0].transaction, expected);
        }

        TEST(LedgerBuilderTest, BuildsCanonicalReleaseAcceptedByLedger) {
            const LedgerTransaction transaction =
                make_release_ledger_transaction(101, 1, 10, 300);
            const LedgerTransaction expected{
                ReleaseLedgerMetadata{101},
                {
                    Posting{
                        1,
                        10,
                        BalanceBucket::Reserved,
                        -300},
                    Posting{
                        1,
                        10,
                        BalanceBucket::Available,
                        300},
                },
            };

            EXPECT_EQ(transaction, expected);
            Ledger ledger;
            EXPECT_NO_THROW(ledger.append(transaction));
            EXPECT_EQ(ledger.entries()[0].transaction, expected);
        }

        TEST(LedgerBuilderTest, BuildsCanonicalTradeFromFinancialConversion) {
            constexpr InstrumentContext instrument{1, 2, 1'000, 1, 1};
            constexpr Trade trade{101, 202, 95, 3, 11};
            const LedgerTransaction transaction =
                make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22);
            const LedgerTransaction expected{
                TradeLedgerMetadata{trade},
                {
                    Posting{
                        11,
                        2,
                        BalanceBucket::Reserved,
                        -285},
                    Posting{
                        22,
                        1,
                        BalanceBucket::Reserved,
                        -3'000},
                    Posting{
                        11,
                        1,
                        BalanceBucket::Available,
                        3'000},
                    Posting{
                        22,
                        2,
                        BalanceBucket::Available,
                        285},
                },
            };

            EXPECT_EQ(transaction, expected);
            Ledger ledger;
            EXPECT_NO_THROW(ledger.append(transaction));
            EXPECT_EQ(ledger.entries()[0].transaction, expected);
        }

        TEST(LedgerBuilderTest, UsesRationalQuoteAndScaledBaseConversion) {
            constexpr InstrumentContext instrument{1, 2, 1'000, 3, 2};
            constexpr Trade trade{101, 202, 4, 5, 17};

            const LedgerTransaction transaction =
                make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22);

            ASSERT_EQ(transaction.postings.size(), 4U);
            EXPECT_EQ(transaction.postings[0].delta, -30);
            EXPECT_EQ(transaction.postings[1].delta, -5'000);
            EXPECT_EQ(transaction.postings[2].delta, 5'000);
            EXPECT_EQ(transaction.postings[3].delta, 30);
            Ledger ledger;
            EXPECT_NO_THROW(ledger.append(transaction));
        }

        TEST(LedgerBuilderTest, RejectsInvalidReserveAndReleaseInputs) {
            EXPECT_THROW(
                static_cast<void>(
                    make_reserve_ledger_transaction(0, 1, 10, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_reserve_ledger_transaction(101, 0, 10, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_reserve_ledger_transaction(101, 1, 0, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_reserve_ledger_transaction(101, 1, 10, 0)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_reserve_ledger_transaction(101, 1, 10, -1)),
                std::invalid_argument);

            EXPECT_THROW(
                static_cast<void>(
                    make_release_ledger_transaction(0, 1, 10, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_release_ledger_transaction(101, 0, 10, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_release_ledger_transaction(101, 1, 0, 300)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_release_ledger_transaction(101, 1, 10, 0)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_release_ledger_transaction(101, 1, 10, -1)),
                std::invalid_argument);
        }

        TEST(LedgerBuilderTest, RejectsInvalidTradeOwnership) {
            constexpr InstrumentContext instrument{1, 2, 1, 1, 1};
            constexpr Trade trade{101, 202, 95, 3, 11};

            EXPECT_THROW(
                static_cast<void>(make_trade_ledger_transaction(
                    instrument,
                    trade,
                    0,
                    22)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    0)),
                std::invalid_argument);
        }

        TEST(LedgerBuilderTest, PropagatesExactDivisibilityFailure) {
            constexpr InstrumentContext instrument{1, 2, 1, 1, 2};
            constexpr Trade trade{101, 202, 3, 1, 11};

            EXPECT_THROW(
                static_cast<void>(make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22)),
                std::invalid_argument);
        }

        TEST(LedgerBuilderTest, PropagatesFinancialConversionOverflow) {
            constexpr Amount maximum = std::numeric_limits<Amount>::max();
            constexpr InstrumentContext instrument{1, 2, 1, maximum, 1};
            constexpr Trade trade{101, 202, 2, 1, 11};

            EXPECT_THROW(
                static_cast<void>(make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22)),
                std::overflow_error);
        }

        TEST(LedgerBuilderTest, AllowsSelfTradeAndPreservesExactTradeMetadata) {
            constexpr InstrumentContext instrument{1, 2, 1, 1, 1};
            constexpr Trade trade{101, 202, 95, 3, 77};

            const LedgerTransaction transaction =
                make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    11);

            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(transaction.metadata).trade,
                trade);
            for (const Posting& posting : transaction.postings) {
                EXPECT_EQ(posting.account_id, 11U);
            }
            Ledger ledger;
            EXPECT_NO_THROW(ledger.append(transaction));
        }

        TEST(LedgerBuilderTest, SameInputsProduceIdenticalTransactions) {
            constexpr InstrumentContext instrument{1, 2, 1'000, 3, 2};
            constexpr Trade trade{101, 202, 4, 5, 17};

            EXPECT_EQ(
                make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22),
                make_trade_ledger_transaction(
                    instrument,
                    trade,
                    11,
                    22));
            EXPECT_EQ(
                make_reserve_ledger_transaction(101, 11, 2, 300),
                make_reserve_ledger_transaction(101, 11, 2, 300));
            EXPECT_EQ(
                make_release_ledger_transaction(101, 11, 2, 15),
                make_release_ledger_transaction(101, 11, 2, 15));
        }

        TEST(LedgerBuilderTest, ComposesPriceImprovementAsTradesThenRelease) {
            constexpr InstrumentContext instrument{1, 2, 1, 1, 1};
            const std::vector<Trade> trades{
                Trade{101, 201, 90, 1, 21},
                Trade{101, 202, 95, 1, 21},
                Trade{101, 203, 100, 1, 21},
            };
            std::vector<LedgerTransaction> transactions;
            transactions.push_back(
                make_reserve_ledger_transaction(101, 11, 2, 300));
            for (std::size_t index = 0; index < trades.size(); ++index) {
                transactions.push_back(make_trade_ledger_transaction(
                    instrument,
                    trades[index],
                    11,
                    static_cast<AccountId>(20 + index)));
            }
            transactions.push_back(
                make_release_ledger_transaction(101, 11, 2, 15));

            ASSERT_EQ(transactions.size(), 5U);
            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(transactions[1].metadata).trade,
                trades[0]);
            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(transactions[2].metadata).trade,
                trades[1]);
            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(transactions[3].metadata).trade,
                trades[2]);
            EXPECT_TRUE(std::holds_alternative<ReleaseLedgerMetadata>(
                transactions[4].metadata));
            EXPECT_EQ(transactions[1].postings[0].delta, -90);
            EXPECT_EQ(transactions[2].postings[0].delta, -95);
            EXPECT_EQ(transactions[3].postings[0].delta, -100);
            EXPECT_EQ(transactions[4].postings[0].delta, -15);

            Ledger ledger;
            EXPECT_NO_THROW(ledger.append_batch(transactions));
            ASSERT_EQ(ledger.entries().size(), 5U);
            for (std::size_t index = 0; index < ledger.entries().size(); ++index) {
                EXPECT_EQ(ledger.entries()[index].sequence, index + 1);
            }
        }

        TEST(LedgerReplayTest, ReserveMatchesDirectAccountStoreMutation) {
            AccountStore actual;
            AccountStore replayed;
            ASSERT_TRUE(actual.create_account(1));
            ASSERT_TRUE(replayed.create_account(1));
            // Genesis funding is intentionally external to Ledger history.
            actual.fund(1, 10, 1'000);
            replayed.fund(1, 10, 1'000);

            ASSERT_EQ(actual.reserve(1, 10, 300), ReserveResult::Success);
            Ledger ledger;
            ledger.append(
                make_reserve_ledger_transaction(101, 1, 10, 300));
            replay_ledger_into_account_store(ledger, replayed);

            expect_balances_equal(actual, replayed, {1}, {10});
            EXPECT_EQ(replayed.find_balance(1, 10), (Balance{700, 300}));
        }

        TEST(LedgerReplayTest, ReserveAndReleaseMatchPartialLifecycle) {
            AccountStore actual;
            AccountStore replayed;
            ASSERT_TRUE(actual.create_account(1));
            ASSERT_TRUE(replayed.create_account(1));
            actual.fund(1, 10, 1'000);
            replayed.fund(1, 10, 1'000);

            ASSERT_EQ(actual.reserve(1, 10, 300), ReserveResult::Success);
            actual.release(1, 10, 120);
            Ledger ledger;
            ledger.append_batch({
                make_reserve_ledger_transaction(101, 1, 10, 300),
                make_release_ledger_transaction(101, 1, 10, 120),
            });
            replay_ledger_into_account_store(ledger, replayed);

            expect_balances_equal(actual, replayed, {1}, {10});
            EXPECT_EQ(replayed.find_balance(1, 10), (Balance{820, 180}));
        }

        TEST(LedgerReplayTest, ManualSingleTradeReconstructsAllBalances) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            constexpr Trade trade{202, 101, 100, 2, 9};
            AccountStore actual;
            AccountStore replayed;
            for (AccountStore* store : {&actual, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 20, 5);
                store->fund(2, 10, 1'000);
            }

            ASSERT_EQ(actual.reserve(1, 20, 2), ReserveResult::Success);
            ASSERT_EQ(actual.reserve(2, 10, 200), ReserveResult::Success);
            actual.consume_reserved(2, 10, 200);
            actual.consume_reserved(1, 20, 2);
            actual.credit_available(2, 20, 2);
            actual.credit_available(1, 10, 200);

            Ledger ledger;
            ledger.append_batch({
                make_reserve_ledger_transaction(101, 1, 20, 2),
                make_reserve_ledger_transaction(202, 2, 10, 200),
                make_trade_ledger_transaction(instrument, trade, 2, 1),
            });
            replay_ledger_into_account_store(ledger, replayed);

            expect_balances_equal(actual, replayed, {1, 2}, {10, 20});
            EXPECT_EQ(replayed.find_balance(2, 20), (Balance{2, 0}));
            EXPECT_EQ(replayed.find_balance(1, 10), (Balance{200, 0}));
        }

        TEST(LedgerReplayTest, NonCrossingAdmissionNeedsOnlyReserveHistory) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            ASSERT_TRUE(runtime.accounts.create_account(1));
            ASSERT_TRUE(replayed.create_account(1));
            runtime.accounts.fund(1, 10, 1'000);
            replayed.fund(1, 10, 1'000);

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(301, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);
            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(runtime.accounts, replayed, {1}, {10, 20});
            ASSERT_EQ(runtime.ledger.entries().size(), 1U);
            EXPECT_EQ(
                runtime.ledger.entries()[0].transaction,
                make_reserve_ledger_transaction(301, 1, 10, 300));
        }

        TEST(LedgerReplayTest,
             RuntimeRestingCancelReplaysReserveAndRelease) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                store->fund(1, 10, 1'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(302, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.cancel_order(1, 302),
                CancelResult::Cancelled);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(runtime.accounts, replayed, {1}, {10, 20});
            EXPECT_EQ(runtime.accounts.find_balance(1, 10),
                      (Balance{1'000, 0}));
            ASSERT_EQ(runtime.ledger.entries().size(), 2U);
            EXPECT_EQ(
                runtime.ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(302, 1, 10, 300)}));
            EXPECT_EQ(
                runtime.ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(302, 1, 10, 300)}));
        }

        TEST(LedgerReplayTest,
             RuntimeScaledSellCancelReplaysNormalizedRelease) {
            constexpr InstrumentContext instrument{
                20,
                10,
                1'000,
                1,
                1,
            };
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                store->fund(1, 20, 4'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(303, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.cancel_order(1, 303),
                CancelResult::Cancelled);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(runtime.accounts, replayed, {1}, {10, 20});
            EXPECT_EQ(runtime.accounts.find_balance(1, 20),
                      (Balance{4'000, 0}));
            ASSERT_EQ(runtime.ledger.entries().size(), 2U);
            EXPECT_EQ(
                runtime.ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(
                        303,
                        1,
                        20,
                        3'000)}));
            EXPECT_EQ(
                runtime.ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_release_ledger_transaction(
                        303,
                        1,
                        20,
                        3'000)}));
        }

        TEST(LedgerReplayTest, ActualCoordinatorCrossingMatchesLedgerProjection) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 20, 5);
                store->fund(2, 10, 1'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(401, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(402, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2},
                {10, 20});
            EXPECT_TRUE(replayed.find_balance(1, 10).has_value());
            EXPECT_TRUE(replayed.find_balance(2, 20).has_value());
            ASSERT_EQ(runtime.ledger.entries().size(), 3U);
            EXPECT_EQ(
                runtime.ledger.entries()[0].transaction,
                make_reserve_ledger_transaction(401, 1, 20, 2));
            EXPECT_EQ(
                runtime.ledger.entries()[1].transaction,
                make_reserve_ledger_transaction(402, 2, 10, 200));
            EXPECT_EQ(
                runtime.ledger.entries()[2].transaction,
                make_trade_ledger_transaction(
                    instrument,
                    Trade{402, 401, 100, 2, 0},
                    2,
                    1));
        }

        TEST(LedgerReplayTest,
             RuntimeSelfTradeLedgerReplayMatchesBalances) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                store->fund(1, 20, 5);
                store->fund(1, 10, 1'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(451, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(452, Side::Buy, 100, 2)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(runtime.accounts, replayed, {1}, {10, 20});
            EXPECT_EQ(runtime.accounts.find_balance(1, 10),
                      (Balance{1'000, 0}));
            EXPECT_EQ(runtime.accounts.find_balance(1, 20),
                      (Balance{5, 0}));
            ASSERT_EQ(runtime.ledger.entries().size(), 3U);
            EXPECT_EQ(
                runtime.ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(451, 1, 20, 2)}));
            EXPECT_EQ(
                runtime.ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_reserve_ledger_transaction(452, 1, 10, 200)}));
            const LedgerTransaction expected_trade =
                make_trade_ledger_transaction(
                    instrument,
                    Trade{452, 451, 100, 2, 0},
                    1,
                    1);
            EXPECT_EQ(
                runtime.ledger.entries()[2],
                (LedgerEntry{3, expected_trade}));

            Amount base_delta = 0;
            Amount quote_delta = 0;
            for (const Posting& posting : expected_trade.postings) {
                if (posting.asset_id == instrument.base_asset) {
                    base_delta += posting.delta;
                } else if (posting.asset_id == instrument.quote_asset) {
                    quote_delta += posting.delta;
                }
            }
            EXPECT_EQ(base_delta, 0);
            EXPECT_EQ(quote_delta, 0);
        }

        TEST(LedgerReplayTest, MultiMakerCrossingPreservesTradeAndResidualOrder) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                ASSERT_TRUE(store->create_account(3));
                store->fund(1, 20, 1);
                store->fund(2, 20, 3);
                store->fund(3, 10, 1'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(501, Side::Sell, 90, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(502, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    3,
                    limit_order(503, Side::Buy, 110, 3)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2, 3},
                {10, 20});
            EXPECT_EQ(replayed.find_balance(2, 20), (Balance{0, 1}));
            ASSERT_EQ(runtime.ledger.entries().size(), 6U);
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                runtime.ledger.entries()[3].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                runtime.ledger.entries()[4].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<ReleaseLedgerMetadata>(
                runtime.ledger.entries()[5].transaction.metadata));
            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(
                    runtime.ledger.entries()[3].transaction.metadata).trade,
                (Trade{503, 501, 90, 1, 0}));
            EXPECT_EQ(
                std::get<TradeLedgerMetadata>(
                    runtime.ledger.entries()[4].transaction.metadata).trade,
                (Trade{503, 502, 100, 2, 0}));
        }

        TEST(LedgerReplayTest, PriceImprovementReplaysTradesAndSeparateResidual) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                for (const AccountId account_id : {1U, 2U, 3U, 4U}) {
                    ASSERT_TRUE(store->create_account(account_id));
                }
                store->fund(1, 20, 1);
                store->fund(2, 20, 1);
                store->fund(3, 20, 1);
                store->fund(4, 10, 1'000);
            }

            for (const OrderAdmissionRequest& request : {
                     OrderAdmissionRequest{
                         1,
                         limit_order(601, Side::Sell, 90, 1)},
                     OrderAdmissionRequest{
                         2,
                         limit_order(602, Side::Sell, 95, 1)},
                     OrderAdmissionRequest{
                         3,
                         limit_order(603, Side::Sell, 100, 1)}}) {
                ASSERT_EQ(
                    runtime.coordinator.submit_order(request),
                    SubmitResult::Accepted);
            }
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    4,
                    limit_order(604, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2, 3, 4},
                {10, 20});
            EXPECT_EQ(replayed.find_balance(4, 10), (Balance{715, 0}));
            EXPECT_EQ(replayed.find_balance(4, 20), (Balance{3, 0}));
            ASSERT_EQ(runtime.ledger.entries().size(), 8U);
            EXPECT_EQ(
                runtime.ledger.entries()[3].transaction,
                make_reserve_ledger_transaction(604, 4, 10, 300));
            const std::array<Trade, 3> expected_trades{
                Trade{604, 601, 90, 1, 0},
                Trade{604, 602, 95, 1, 0},
                Trade{604, 603, 100, 1, 0},
            };
            for (std::size_t index = 0;
                 index < expected_trades.size();
                 ++index) {
                EXPECT_EQ(
                    std::get<TradeLedgerMetadata>(
                        runtime.ledger.entries()[4 + index]
                            .transaction.metadata).trade,
                    expected_trades[index]);
            }
            EXPECT_EQ(
                runtime.ledger.entries()[7].transaction,
                make_release_ledger_transaction(604, 4, 10, 15));
        }

        TEST(LedgerReplayTest,
             RestingBuyMakerResidualReleaseIsRuntimeReplayable) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                for (const AccountId account_id : {1U, 2U, 3U}) {
                    ASSERT_TRUE(store->create_account(account_id));
                }
                store->fund(1, 20, 1);
                store->fund(2, 10, 500);
                store->fund(3, 20, 2);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(651, Side::Sell, 90, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(652, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.reservations.find(652),
                (OrderReservation{2, 10, 300, 210}));
            ASSERT_TRUE(
                runtime.matching_engine.order_book().find_order(652)
                    .has_value());
            EXPECT_EQ(
                runtime.matching_engine.order_book().find_order(652)->quantity,
                2);
            EXPECT_EQ(runtime.accounts.find_balance(2, 10),
                      (Balance{200, 210}));

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    3,
                    limit_order(653, Side::Sell, 100, 2)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2, 3},
                {10, 20});
            EXPECT_EQ(runtime.accounts.find_balance(2, 10),
                      (Balance{210, 0}));
            EXPECT_EQ(runtime.accounts.find_balance(2, 20),
                      (Balance{3, 0}));
            EXPECT_FALSE(runtime.reservations.find(652).has_value());
            EXPECT_FALSE(
                runtime.matching_engine.order_book().find_order(652)
                    .has_value());

            const std::array<LedgerTransaction, 6> expected{
                make_reserve_ledger_transaction(651, 1, 20, 1),
                make_reserve_ledger_transaction(652, 2, 10, 300),
                make_trade_ledger_transaction(
                    instrument,
                    Trade{652, 651, 90, 1, 0},
                    2,
                    1),
                make_reserve_ledger_transaction(653, 3, 20, 2),
                make_trade_ledger_transaction(
                    instrument,
                    Trade{652, 653, 100, 2, 0},
                    2,
                    3),
                make_release_ledger_transaction(652, 2, 10, 10),
            };
            ASSERT_EQ(runtime.ledger.entries().size(), expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                EXPECT_EQ(runtime.ledger.entries()[index],
                          (LedgerEntry{index + 1, expected[index]}));
            }
        }

        TEST(LedgerReplayTest, PartialFillThenCancelReleasesOnlyRemainder) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 20, 1);
                store->fund(2, 10, 500);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(701, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(702, Side::Buy, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.cancel_order(2, 702),
                CancelResult::Cancelled);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2},
                {10, 20});
            EXPECT_EQ(replayed.find_balance(2, 10), (Balance{400, 0}));
            ASSERT_EQ(runtime.ledger.entries().size(), 4U);
            EXPECT_EQ(
                runtime.ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(701, 1, 20, 1)}));
            EXPECT_EQ(
                runtime.ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_reserve_ledger_transaction(702, 2, 10, 300)}));
            EXPECT_EQ(
                runtime.ledger.entries()[2],
                (LedgerEntry{
                    3,
                    make_trade_ledger_transaction(
                        instrument,
                        Trade{702, 701, 100, 1, 0},
                        2,
                        1)}));
            EXPECT_EQ(
                runtime.ledger.entries()[3],
                (LedgerEntry{
                    4,
                    make_release_ledger_transaction(702, 2, 10, 200)}));
        }

        TEST(LedgerReplayTest,
             PartialSellThenCancelLedgerReplayMatchesBalances) {
            constexpr InstrumentContext instrument{
                20,
                10,
                1'000,
                1,
                1,
            };
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 10, 1'000);
                store->fund(2, 20, 5'000);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(751, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(752, Side::Sell, 100, 3)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.reservations.find(752),
                (OrderReservation{2, 20, 3'000, 2'000}));
            ASSERT_EQ(
                runtime.coordinator.cancel_order(2, 752),
                CancelResult::Cancelled);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2},
                {10, 20});
            EXPECT_EQ(runtime.accounts.find_balance(1, 10),
                      (Balance{900, 0}));
            EXPECT_EQ(runtime.accounts.find_balance(1, 20),
                      (Balance{1'000, 0}));
            EXPECT_EQ(runtime.accounts.find_balance(2, 10),
                      (Balance{100, 0}));
            EXPECT_EQ(runtime.accounts.find_balance(2, 20),
                      (Balance{4'000, 0}));
            EXPECT_FALSE(runtime.reservations.find(752).has_value());

            const std::array<LedgerTransaction, 4> expected{
                make_reserve_ledger_transaction(751, 1, 10, 100),
                make_reserve_ledger_transaction(752, 2, 20, 3'000),
                make_trade_ledger_transaction(
                    instrument,
                    Trade{751, 752, 100, 1, 0},
                    1,
                    2),
                make_release_ledger_transaction(752, 2, 20, 2'000),
            };
            ASSERT_EQ(runtime.ledger.entries().size(), expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                EXPECT_EQ(runtime.ledger.entries()[index],
                          (LedgerEntry{index + 1, expected[index]}));
            }
        }

        TEST(LedgerReplayTest,
             RuntimeScaledRationalTradeLedgerReplayMatchesBalances) {
            constexpr InstrumentContext instrument{1, 2, 1'000, 3, 2};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 1, 5'000);
                store->fund(2, 2, 30);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(801, Side::Sell, 4, 5)}),
                SubmitResult::Accepted);
            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(802, Side::Buy, 4, 5)}),
                SubmitResult::Accepted);

            replay_ledger_into_account_store(runtime.ledger, replayed);

            expect_balances_equal(runtime.accounts, replayed, {1, 2}, {1, 2});
            EXPECT_EQ(replayed.find_balance(1, 1), (Balance{0, 0}));
            EXPECT_EQ(replayed.find_balance(1, 2), (Balance{30, 0}));
            EXPECT_EQ(replayed.find_balance(2, 1), (Balance{5'000, 0}));
            EXPECT_EQ(replayed.find_balance(2, 2), (Balance{0, 0}));
            EXPECT_TRUE(replayed.find_balance(2, 1).has_value());
            EXPECT_TRUE(replayed.find_balance(1, 2).has_value());
            ASSERT_EQ(runtime.ledger.entries().size(), 3U);
            EXPECT_EQ(
                runtime.ledger.entries()[0],
                (LedgerEntry{
                    1,
                    make_reserve_ledger_transaction(
                        801,
                        1,
                        1,
                        5'000)}));
            EXPECT_EQ(
                runtime.ledger.entries()[1],
                (LedgerEntry{
                    2,
                    make_reserve_ledger_transaction(802, 2, 2, 30)}));
            EXPECT_EQ(
                runtime.ledger.entries()[2],
                (LedgerEntry{
                    3,
                    make_trade_ledger_transaction(
                        instrument,
                        Trade{802, 801, 4, 5, 0},
                        2,
                        1)}));
        }

        TEST(LedgerReplayTest,
             MixedSuccessFailureOperationsPreserveContinuousLedgerSequence) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            CoordinatorHarness runtime{instrument};
            AccountStore replayed;
            for (AccountStore* store : {&runtime.accounts, &replayed}) {
                ASSERT_TRUE(store->create_account(1));
                ASSERT_TRUE(store->create_account(2));
                store->fund(1, 10, 1'000);
                store->fund(2, 20, 1);
            }

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(851, Side::Buy, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(runtime.ledger.entries().size(), 1U);

            const auto before_insufficient = runtime.ledger.entries();
            EXPECT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(852, Side::Buy, 1'000, 1)}),
                SubmitResult::InsufficientFunds);
            EXPECT_EQ(runtime.ledger.entries(), before_insufficient);

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    2,
                    limit_order(853, Side::Sell, 100, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(runtime.ledger.entries().size(), 3U);

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(854, Side::Buy, 90, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(runtime.ledger.entries().size(), 4U);

            const auto before_not_owner = runtime.ledger.entries();
            EXPECT_EQ(
                runtime.coordinator.cancel_order(2, 854),
                CancelResult::NotOwner);
            EXPECT_EQ(runtime.ledger.entries(), before_not_owner);

            const auto before_not_found = runtime.ledger.entries();
            EXPECT_EQ(
                runtime.coordinator.cancel_order(1, 899),
                CancelResult::NotFound);
            EXPECT_EQ(runtime.ledger.entries(), before_not_found);

            ASSERT_EQ(
                runtime.coordinator.cancel_order(1, 854),
                CancelResult::Cancelled);
            ASSERT_EQ(runtime.ledger.entries().size(), 5U);

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(855, Side::Buy, 80, 1)}),
                SubmitResult::Accepted);
            ASSERT_EQ(runtime.ledger.entries().size(), 6U);

            const auto before_duplicate = runtime.ledger.entries();
            EXPECT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(855, Side::Buy, 70, 1)}),
                SubmitResult::DuplicateOrder);
            EXPECT_EQ(runtime.ledger.entries(), before_duplicate);

            ASSERT_EQ(
                runtime.coordinator.submit_order(OrderAdmissionRequest{
                    1,
                    limit_order(856, Side::Buy, 70, 1)}),
                SubmitResult::Accepted);

            const std::array<LedgerTransaction, 7> expected{
                make_reserve_ledger_transaction(851, 1, 10, 100),
                make_reserve_ledger_transaction(853, 2, 20, 1),
                make_trade_ledger_transaction(
                    instrument,
                    Trade{851, 853, 100, 1, 0},
                    1,
                    2),
                make_reserve_ledger_transaction(854, 1, 10, 90),
                make_release_ledger_transaction(854, 1, 10, 90),
                make_reserve_ledger_transaction(855, 1, 10, 80),
                make_reserve_ledger_transaction(856, 1, 10, 70),
            };
            ASSERT_EQ(runtime.ledger.entries().size(), expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                EXPECT_EQ(runtime.ledger.entries()[index].sequence,
                          index + 1);
                EXPECT_EQ(runtime.ledger.entries()[index].transaction,
                          expected[index]);
            }

            replay_ledger_into_account_store(runtime.ledger, replayed);
            expect_balances_equal(
                runtime.accounts,
                replayed,
                {1, 2},
                {10, 20});
        }

        TEST(LedgerReplayTest, ReplayIsDeterministicAndLeavesHistoryUnchanged) {
            Ledger ledger;
            ledger.append_batch({
                make_reserve_ledger_transaction(901, 1, 10, 300),
                make_release_ledger_transaction(901, 1, 10, 120),
            });
            const auto history_before = ledger.entries();
            AccountStore first;
            AccountStore second;
            for (AccountStore* store : {&first, &second}) {
                ASSERT_TRUE(store->create_account(1));
                store->fund(1, 10, 1'000);
            }

            replay_ledger_into_account_store(ledger, first);
            replay_ledger_into_account_store(ledger, second);

            expect_balances_equal(first, second, {1}, {10});
            EXPECT_EQ(ledger.entries(), history_before);
        }

        TEST(LedgerFundingBuilderTest,
             BuildsCanonicalDeterministicBalancedFunding) {
            const LedgerTransaction expected = funding_transaction();
            const LedgerTransaction first =
                make_funding_ledger_transaction(10, 20, 7, 500);
            const LedgerTransaction second =
                make_funding_ledger_transaction(10, 20, 7, 500);

            EXPECT_EQ(first, expected);
            EXPECT_EQ(second, first);
            ASSERT_TRUE(std::holds_alternative<FundingLedgerMetadata>(
                first.metadata));
            EXPECT_EQ(
                std::get<FundingLedgerMetadata>(first.metadata),
                (FundingLedgerMetadata{10, 20}));
            ASSERT_EQ(first.postings.size(), 2U);
            EXPECT_EQ(first.postings[0],
                      (Posting{10,
                               7,
                               BalanceBucket::Available,
                               -500}));
            EXPECT_EQ(first.postings[1],
                      (Posting{20,
                               7,
                               BalanceBucket::Available,
                               500}));
            EXPECT_EQ(
                first.postings[0].delta + first.postings[1].delta,
                0);

            const LedgerTransaction other_asset =
                make_funding_ledger_transaction(30, 40, 99, 700);
            EXPECT_EQ(other_asset.postings[0].asset_id, 99U);
            EXPECT_EQ(other_asset.postings[1].asset_id, 99U);

            const Amount maximum = std::numeric_limits<Amount>::max();
            const LedgerTransaction large =
                make_funding_ledger_transaction(10, 20, 7, maximum);
            EXPECT_EQ(large.postings[0].delta, -maximum);
            EXPECT_EQ(large.postings[1].delta, maximum);

            Ledger ledger;
            ledger.append(first);
            ASSERT_EQ(ledger.entries().size(), 1U);
            EXPECT_EQ(ledger.entries()[0], (LedgerEntry{1, first}));
        }

        TEST(LedgerFundingBuilderTest,
             RejectsInvalidInputsWithoutMutatingLedger) {
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(0, 20, 7, 500)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(10, 0, 7, 500)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(10, 10, 7, 500)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(10, 20, 0, 500)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(10, 20, 7, 0)),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(
                    make_funding_ledger_transaction(10, 20, 7, -1)),
                std::invalid_argument);

            Ledger ledger;
            EXPECT_TRUE(ledger.entries().empty());
        }

        TEST(LedgerFundingTest, RejectsEveryMalformedCanonicalShape) {
            const auto expect_invalid = [](LedgerTransaction transaction) {
                Ledger ledger;
                EXPECT_THROW(
                    ledger.append(std::move(transaction)),
                    std::invalid_argument);
                EXPECT_TRUE(ledger.entries().empty());
            };

            LedgerTransaction wrong_count = funding_transaction();
            wrong_count.postings.push_back(wrong_count.postings[1]);
            expect_invalid(std::move(wrong_count));

            LedgerTransaction reversed = funding_transaction();
            std::swap(reversed.postings[0], reversed.postings[1]);
            expect_invalid(std::move(reversed));

            LedgerTransaction source_reserved = funding_transaction();
            source_reserved.postings[0].bucket = BalanceBucket::Reserved;
            expect_invalid(std::move(source_reserved));

            LedgerTransaction destination_reserved = funding_transaction();
            destination_reserved.postings[1].bucket = BalanceBucket::Reserved;
            expect_invalid(std::move(destination_reserved));

            LedgerTransaction different_assets = funding_transaction();
            different_assets.postings[1].asset_id = 8;
            expect_invalid(std::move(different_assets));

            LedgerTransaction same_account = funding_transaction();
            same_account.metadata = FundingLedgerMetadata{10, 10};
            same_account.postings[1].account_id = 10;
            expect_invalid(std::move(same_account));

            LedgerTransaction source_positive = funding_transaction();
            source_positive.postings[0].delta = 500;
            source_positive.postings[1].delta = -500;
            expect_invalid(std::move(source_positive));

            LedgerTransaction destination_negative = funding_transaction();
            destination_negative.postings[1].delta = -500;
            expect_invalid(std::move(destination_negative));

            LedgerTransaction unequal = funding_transaction();
            unequal.postings[1].delta = 499;
            expect_invalid(std::move(unequal));

            LedgerTransaction zero_account = funding_transaction();
            zero_account.metadata = FundingLedgerMetadata{0, 20};
            zero_account.postings[0].account_id = 0;
            expect_invalid(std::move(zero_account));

            LedgerTransaction zero_asset = funding_transaction();
            zero_asset.postings[0].asset_id = 0;
            zero_asset.postings[1].asset_id = 0;
            expect_invalid(std::move(zero_asset));

            LedgerTransaction metadata_mismatch = funding_transaction();
            metadata_mismatch.metadata = FundingLedgerMetadata{11, 20};
            expect_invalid(std::move(metadata_mismatch));

            LedgerTransaction zero_delta = funding_transaction();
            zero_delta.postings[0].delta = 0;
            expect_invalid(std::move(zero_delta));

            LedgerTransaction minimum_delta = funding_transaction();
            minimum_delta.postings[0].delta =
                std::numeric_limits<Amount>::min();
            expect_invalid(std::move(minimum_delta));
        }

        TEST(LedgerFundingTest,
             ParticipatesInNormalAppendBatchSequenceOrdering) {
            Ledger ledger;
            const LedgerTransaction reserve = reserve_transaction();
            const LedgerTransaction funding = funding_transaction();
            const LedgerTransaction trade = trade_transaction();
            const LedgerTransaction release = release_transaction();

            ledger.append_batch({reserve, funding, trade, release});

            ASSERT_EQ(ledger.entries().size(), 4U);
            EXPECT_EQ(ledger.entries()[0], (LedgerEntry{1, reserve}));
            EXPECT_EQ(ledger.entries()[1], (LedgerEntry{2, funding}));
            EXPECT_EQ(ledger.entries()[2], (LedgerEntry{3, trade}));
            EXPECT_EQ(ledger.entries()[3], (LedgerEntry{4, release}));
        }

        TEST(LedgerFundingTest,
             PreparedBatchKeepsFundingInvisibleUntilCommitAndPreservesOrder) {
            Ledger ledger;
            const LedgerTransaction funding = funding_transaction();
            const LedgerTransaction reserve = reserve_transaction();
            const LedgerTransaction trade = trade_transaction();

            auto prepared = ledger.prepare_batch(
                {funding, reserve, trade});
            EXPECT_TRUE(ledger.entries().empty());

            prepared.commit();

            ASSERT_EQ(ledger.entries().size(), 3U);
            EXPECT_EQ(ledger.entries()[0], (LedgerEntry{1, funding}));
            EXPECT_EQ(ledger.entries()[1], (LedgerEntry{2, reserve}));
            EXPECT_EQ(ledger.entries()[2], (LedgerEntry{3, trade}));
        }

        TEST(LedgerTest, ExposesHistoryAsConstReferenceOnly) {
            static_assert(std::is_same_v<
                          decltype(std::declval<const Ledger&>().entries()),
                          const std::vector<LedgerEntry>&>);

            Ledger ledger;
            ledger.append(reserve_transaction());
            const auto& entries = ledger.entries();

            ASSERT_EQ(entries.size(), 1U);
            EXPECT_EQ(entries.front().sequence, 1U);
        }
    }  // namespace
}  // namespace exchange
