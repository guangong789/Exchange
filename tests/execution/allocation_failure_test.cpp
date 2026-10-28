#include "../support/wal_files.hpp"
#include "accounting/execution_coordinator.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"

#include <cstdlib>
#include <memory>
#include <new>

#include <gtest/gtest.h>

// This replacement allocator is linked only into exchange_allocation_test.
// Failure is thread-local, armed only around the operation under test, and
// automatically disarmed at the first injected failure (including unwinding).
namespace {
    thread_local long allocations_before_failure = -1;

    class FailAllocation {
      public:
        explicit FailAllocation(long successful_allocations) noexcept {
            allocations_before_failure = successful_allocations;
        }
        ~FailAllocation() { allocations_before_failure = -1; }
        FailAllocation(const FailAllocation &) = delete;
        FailAllocation &operator=(const FailAllocation &) = delete;
    };
} // namespace

void *operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void *memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{1, 2, 1, 1, 1};

        TEST(OrderBookAllocationTest, FailedRestInsertionPreservesIndexAndExecutableOrders) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                for (const bool same_level : {false, true}) {
                    SCOPED_TRACE(static_cast<int>(side));
                    SCOPED_TRACE(same_level);
                    bool succeeded = false;
                    long failures = 0;
                    // Sweep every allocation until the first complete operation.
                    for (long fail_after = 0; fail_after < 64; ++fail_after) {
                        SCOPED_TRACE(fail_after);
                        OrderBook book;
                        static_cast<void>(book.add_order({1, side, OrderType::Limit, 100, 1, 1}));
                        const Price price = same_level ? 100 : (side == Side::Buy ? 101 : 99);
                        bool failed = false;
                        try {
                            FailAllocation injection{fail_after};
                            static_cast<void>(
                                book.add_order({2, side, OrderType::Limit, price, 1, 2}));
                        } catch (const std::bad_alloc &) {
                            failed = true;
                        }
                        if (!failed) {
                            EXPECT_EQ(book.order_count(), 2U);
                            EXPECT_TRUE(book.find_order(2).has_value());
                            succeeded = true;
                            break;
                        }
                        ++failures;
                        EXPECT_EQ(book.order_count(), 1U);
                        EXPECT_TRUE(book.find_order(1).has_value());
                        EXPECT_FALSE(book.find_order(2).has_value());
                        EXPECT_EQ(side == Side::Buy ? book.best_bid() : book.best_ask(), 100);

                        // Exercise executable liquidity, not just the index:
                        // a failed insertion must not leave a hidden maker.
                        const Side opposite = side == Side::Buy ? Side::Sell : Side::Buy;
                        const auto trades =
                            book.add_order({3, opposite, OrderType::Limit, 100, 2, 3});
                        ASSERT_EQ(trades.size(), 1U);
                        EXPECT_EQ(side == Side::Buy ? trades[0].buy_order_id
                                                    : trades[0].sell_order_id,
                                  1U);
                        EXPECT_EQ(trades[0].quantity, 1);
                        ASSERT_TRUE(book.find_order(3).has_value());
                        EXPECT_EQ(book.find_order(3)->quantity, 1);
                        EXPECT_EQ(book.order_count(), 1U);
                    }
                    EXPECT_TRUE(succeeded);
                    EXPECT_GT(failures, 0);
                }
            }
        }

        TEST(CoordinatorAllocationTest, PreviewFailureRollsBackIncomingReservation) {
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, 1, 1);
            accounts.fund(2, 2, 100);
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching{events};
            Ledger ledger;
            ExecutionCoordinator coordinator{instrument, accounts, reservations,
                                             matching,   events,   ledger};
            ASSERT_EQ(coordinator.submit_order({1, {1, Side::Sell, OrderType::Limit, 100, 1, 1}}),
                      SubmitResult::Accepted);
            const auto accounts_before = accounts.entries();
            const auto reservations_before = reservations.entries();
            const auto ledger_before = ledger.entries();
            bool failed = false;
            try {
                // The reservation node succeeds; the first preview vector
                // allocation fails before any matching or financial clearing.
                FailAllocation injection{1};
                static_cast<void>(
                    coordinator.submit_order({2, {2, Side::Buy, OrderType::Limit, 100, 1, 2}}));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
            ASSERT_TRUE(failed);
            EXPECT_EQ(accounts.entries(), accounts_before);
            EXPECT_EQ(reservations.entries(), reservations_before);
            EXPECT_EQ(ledger.entries(), ledger_before);
            ASSERT_TRUE(matching.order_book().find_order(1).has_value());
            EXPECT_EQ(matching.order_book().find_order(1)->quantity, 1);
            EXPECT_FALSE(matching.order_book().find_order(2).has_value());
            EXPECT_TRUE(events.empty());
            // This local pre-matching failure is genuinely rolled back.
            EXPECT_EQ(coordinator.submit_order({2, {2, Side::Buy, OrderType::Limit, 100, 1, 2}}),
                      SubmitResult::Accepted);
        }

        TEST(ExecutorAllocationTest, EventPublicationFailurePoisonsNonDurableExecution) {
            bool exercised_event_failure = false;
            for (long fail_after = 0; fail_after < 64; ++fail_after) {
                AccountStore accounts;
                ASSERT_TRUE(accounts.create_account(1));
                accounts.fund(1, 2, 100);
                OrderReservationStore reservations;
                EventCollector events;
                MatchingEngine matching{events};
                Ledger ledger;
                ExecutionCoordinator coordinator{instrument, accounts, reservations,
                                                 matching,   events,   ledger};
                ExecutionSequencer sequencer;
                ExecutionRuntimeStatus shared_status;
                TradingRequestExecutor executor{instrument, coordinator, events,
                                                sequencer,  nullptr,     &shared_status};
                bool failed = false;
                try {
                    FailAllocation injection{fail_after};
                    static_cast<void>(
                        executor.execute({1, 1, SubmitTradingRequest{Side::Buy, 100, 1}}));
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
                if (!failed) {
                    break;
                }
                if (matching.order_book().find_order(1).has_value() && events.empty()) {
                    // A resting order has no trades. The first allocation after
                    // inserting it is OrderAccepted publication, before Ledger
                    // preparation. This identifies the actual failure boundary.
                    exercised_event_failure = true;
                    EXPECT_TRUE(ledger.entries().empty());
                    EXPECT_TRUE(executor.poisoned());
                    EXPECT_TRUE(shared_status.poisoned);
                    EXPECT_THROW(
                        static_cast<void>(executor.execute({2, 1, CancelTradingRequest{1}})),
                        std::logic_error);
                    break;
                }
            }
            EXPECT_TRUE(exercised_event_failure);
        }

        TEST(RuntimeAllocationTest, UnexpectedApplyFailuresStopBothExecutorsInEitherMode) {
            const TradingBootstrapConfig bootstrap{{{1, {{2, {1'000, 0}}}}, {2, {{1, {1, 0}}}}}};
            for (const bool crossing : {false, true}) {
                SCOPED_TRACE(crossing);
                for (const bool durable : {false, true}) {
                    SCOPED_TRACE(durable);
                    bool succeeded = false;
                    bool saw_post_matching_failure = false;
                    for (long fail_after = 0; fail_after < 128; ++fail_after) {
                        SCOPED_TRACE(fail_after);
                        test::TempTestDirectory directory;
                        const auto path = directory.wal_path();
                        auto runtime =
                            durable ? TradingRuntime::create_durable(instrument, path, bootstrap)
                                    : std::make_unique<TradingRuntime>(instrument);
                        if (!durable) {
                            apply_trading_bootstrap(bootstrap, runtime->accounts());
                        }
                        if (crossing) {
                            ASSERT_EQ(runtime->executor()
                                          .execute({1, 2, SubmitTradingRequest{Side::Sell, 100, 1}})
                                          .result,
                                      TradingResult::Accepted);
                        }
                        bool failed = false;
                        try {
                            FailAllocation injection{fail_after};
                            static_cast<void>(runtime->executor().execute(
                                {2, 1, SubmitTradingRequest{Side::Buy, 100, 1}}));
                        } catch (const std::bad_alloc &) {
                            failed = true;
                        }
                        if (!failed) {
                            EXPECT_FALSE(runtime->executor().poisoned());
                            succeeded = true;
                            break;
                        }
                        saw_post_matching_failure |=
                            crossing ? !runtime->order_book().find_order(1).has_value()
                                     : runtime->order_book().find_order(1).has_value();
                        EXPECT_TRUE(runtime->executor().poisoned());
                        EXPECT_TRUE(runtime->contract_executor().poisoned());
                        EXPECT_THROW(static_cast<void>(runtime->accounts()), std::logic_error);
                        const TradingRuntime &view = *runtime;
                        const auto accounts_before = view.accounts().entries();
                        const auto reservations_before = view.reservations().entries();
                        const auto ledger_before = view.ledger().entries();
                        const auto count_before = view.order_book().order_count();
                        EXPECT_THROW(static_cast<void>(runtime->executor().execute(
                                         {2, 1, SubmitTradingRequest{Side::Buy, 100, 1}})),
                                     std::logic_error);
                        EXPECT_THROW(
                            static_cast<void>(runtime->contract_executor().accept_contract(1, 1)),
                            std::logic_error);
                        EXPECT_EQ(view.accounts().entries(), accounts_before);
                        EXPECT_EQ(view.reservations().entries(), reservations_before);
                        EXPECT_EQ(view.ledger().entries(), ledger_before);
                        EXPECT_EQ(view.order_book().order_count(), count_before);
                        runtime.reset();
                        if (durable) {
                            const auto scan =
                                scan_execution_wal(test::read_file_bytes(path), instrument,
                                                   calculate_bootstrap_fingerprint(bootstrap));
                            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
                            const std::size_t initial_records = crossing ? 1 : 0;
                            ASSERT_GE(scan.records.size(), initial_records);
                            ASSERT_LE(scan.records.size(), initial_records + 1);
                            const bool taker_replayed = scan.records.size() > initial_records;
                            auto recovered =
                                TradingRuntime::create_durable(instrument, path, bootstrap);
                            EXPECT_FALSE(recovered->executor().poisoned());
                            const std::size_t live_orders =
                                crossing ? (taker_replayed ? 0 : 1) : (taker_replayed ? 1 : 0);
                            EXPECT_EQ(recovered->order_book().order_count(), live_orders);
                            EXPECT_EQ(recovered->reservations().entries().size(), live_orders);
                            EXPECT_EQ(recovered->ledger().entries().size(),
                                      crossing && taker_replayed ? 3U : scan.records.size());
                            EXPECT_EQ(static_cast<const TradingRuntime &>(*recovered)
                                          .accounts()
                                          .find_balance(1, 2),
                                      (!taker_replayed ? Balance{1'000, 0}
                                       : crossing      ? Balance{900, 0}
                                                       : Balance{900, 100}));
                            if (crossing && taker_replayed) {
                                EXPECT_EQ(static_cast<const TradingRuntime &>(*recovered)
                                              .accounts()
                                              .find_balance(1, 1),
                                          (Balance{1, 0}));
                                EXPECT_EQ(static_cast<const TradingRuntime &>(*recovered)
                                              .accounts()
                                              .find_balance(2, 2),
                                          (Balance{100, 0}));
                            }
                        }
                    }
                    EXPECT_TRUE(succeeded);
                    EXPECT_TRUE(saw_post_matching_failure);
                }
            }
        }
    } // namespace
} // namespace exchange
