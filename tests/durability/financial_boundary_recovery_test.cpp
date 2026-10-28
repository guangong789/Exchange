#include "../support/wal_files.hpp"
#include "durability/execution_recovery.hpp"
#include "durability/execution_wal_writer.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <limits>
#include <map>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{1, 2, 1, 1, 1};
        constexpr Amount maximum = std::numeric_limits<Amount>::max();

        TradingResponse submit(TradingRuntime &runtime, RequestId request_id, AccountId account_id,
                               Side side, Price price, Quantity quantity) {
            return runtime.executor().execute(
                {request_id, account_id, SubmitTradingRequest{side, price, quantity}});
        }

        TradingResponse cancel(TradingRuntime &runtime, RequestId request_id, AccountId account_id,
                               OrderId order_id) {
            return runtime.executor().execute(
                {request_id, account_id, CancelTradingRequest{order_id}});
        }

        struct Snapshot {
            AccountStore::AccountBalances accounts;
            std::map<OrderId, OrderReservation> reservations;
            std::vector<LedgerEntry> ledger;
            std::vector<Order> orders;
        };

        Snapshot snapshot(const TradingRuntime &runtime) {
            Snapshot state{runtime.accounts().entries(),
                           runtime.reservations().entries(),
                           runtime.ledger().entries(),
                           {}};
            for (const auto &[id, reservation] : state.reservations) {
                static_cast<void>(reservation);
                state.orders.push_back(runtime.order_book().find_order(id).value());
            }
            return state;
        }

        void expect_state(const TradingRuntime &runtime, const Snapshot &expected) {
            EXPECT_EQ(runtime.accounts().entries(), expected.accounts);
            EXPECT_EQ(runtime.reservations().entries(), expected.reservations);
            EXPECT_EQ(runtime.ledger().entries(), expected.ledger);
            EXPECT_EQ(runtime.order_book().order_count(), expected.orders.size());
            for (const auto &order : expected.orders) {
                const auto actual = runtime.order_book().find_order(order.id);
                ASSERT_TRUE(actual.has_value());
                EXPECT_EQ(actual->side, order.side);
                EXPECT_EQ(actual->type, order.type);
                EXPECT_EQ(actual->price, order.price);
                EXPECT_EQ(actual->quantity, order.quantity);
                EXPECT_EQ(actual->timestamp, order.timestamp);
            }
        }

        TradingBootstrapConfig reservation_bootstrap() {
            return {{{1, {{1, {1, 0}}, {2, {maximum, 0}}}}, {2, {{2, {2'000, 0}}}}}};
        }

        TEST(FinancialBoundaryRecoveryTest,
             RejectedCreditPreservesCancellableReservationsAndReplays) {
            test::TempTestDirectory directory;
            const auto bootstrap = reservation_bootstrap();
            auto runtime =
                TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            ASSERT_EQ(submit(*runtime, 1, 1, Side::Buy, 1'000, 2).result, TradingResult::Accepted);
            ASSERT_EQ(submit(*runtime, 2, 1, Side::Sell, 2'000, 1).result, TradingResult::Accepted);
            const auto before = snapshot(*runtime);

            const auto rejected = submit(*runtime, 3, 2, Side::Buy, 2'000, 1);
            EXPECT_EQ(rejected.result, TradingResult::InvalidOrder);
            EXPECT_TRUE(rejected.events.empty());
            EXPECT_FALSE(rejected.assigned_order_id.has_value());
            expect_state(*runtime, before);
            EXPECT_FALSE(runtime->executor().poisoned());
            ASSERT_EQ(cancel(*runtime, 4, 1, 1).result, TradingResult::Cancelled);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum, 0}));
            EXPECT_FALSE(runtime->reservations().find(1).has_value());
            EXPECT_FALSE(runtime->order_book().find_order(1).has_value());
            const auto after = snapshot(*runtime);
            runtime.reset();

            std::vector<RecoveredTradingOutcome> outcomes;
            runtime = TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap,
                                                     &outcomes);
            expect_state(*runtime, after);
            EXPECT_EQ(outcomes,
                      (std::vector<RecoveredTradingOutcome>{{1, TradingResult::Accepted},
                                                            {2, TradingResult::Accepted},
                                                            {3, TradingResult::InvalidOrder},
                                                            {4, TradingResult::Cancelled}}));
            const auto next = submit(*runtime, 5, 2, Side::Buy, 1'000, 1);
            ASSERT_EQ(next.result, TradingResult::Accepted);
            EXPECT_EQ(next.assigned_order_id, 4U);
            EXPECT_EQ(std::get<OrderAccepted>(next.events.front().payload).order.timestamp, 4);
        }

        TEST(FinancialBoundaryRecoveryTest,
             PreviouslyUnreleasableCancellationHistoryRecoversWithoutRewritingWal) {
            test::TempTestDirectory directory;
            const auto bootstrap = reservation_bootstrap();
            const auto path = directory.wal_path();
            {
                // These exact commands could be persisted by the old runtime:
                // credit was accepted, then cancellation threw after journaling.
                ExecutionWalWriter writer{path, instrument,
                                          calculate_bootstrap_fingerprint(bootstrap)};
                writer.append(
                    SubmitExecutionCommand{1, 1, {1, Side::Buy, OrderType::Limit, 1'000, 2, 1}});
                writer.append(
                    SubmitExecutionCommand{2, 1, {2, Side::Sell, OrderType::Limit, 2'000, 1, 2}});
                writer.append(
                    SubmitExecutionCommand{3, 2, {3, Side::Buy, OrderType::Limit, 2'000, 1, 3}});
                writer.append(CancelExecutionCommand{4, 1, 1});
            }
            const auto original_wal = test::read_file_bytes(path);
            std::vector<RecoveredTradingOutcome> outcomes;
            const auto runtime =
                TradingRuntime::create_durable(instrument, path, bootstrap, &outcomes);
            EXPECT_EQ(outcomes,
                      (std::vector<RecoveredTradingOutcome>{{1, TradingResult::Accepted},
                                                            {2, TradingResult::Accepted},
                                                            {3, TradingResult::InvalidOrder},
                                                            {4, TradingResult::Cancelled}}));
            EXPECT_EQ(test::read_file_bytes(path), original_wal);
            EXPECT_EQ(runtime->order_book().order_count(), 1U);
            EXPECT_TRUE(runtime->order_book().find_order(2).has_value());
            EXPECT_FALSE(runtime->reservations().find(1).has_value());
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum, 0}));
            EXPECT_EQ(runtime->ledger().entries().size(), 3U);
        }

        TEST(FinancialBoundaryRecoveryTest, TradingCreditAtExactTotalLimitStillCancelsAndReplays) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig bootstrap{
                {{1, {{1, {1, 0}}, {2, {maximum - 2, 0}}}}, {2, {{2, {2, 0}}}}}};
            auto runtime =
                TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            ASSERT_EQ(submit(*runtime, 1, 1, Side::Buy, 1, 2'000).result, TradingResult::Accepted);
            ASSERT_EQ(submit(*runtime, 2, 1, Side::Sell, 2, 1).result, TradingResult::Accepted);
            ASSERT_EQ(submit(*runtime, 3, 2, Side::Buy, 2, 1).result, TradingResult::Accepted);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum - 2'000, 2'000}));
            ASSERT_EQ(cancel(*runtime, 4, 1, 1).result, TradingResult::Cancelled);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum, 0}));
            const auto after = snapshot(*runtime);
            runtime.reset();
            runtime = TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            expect_state(*runtime, after);
        }

        TEST(FinancialBoundaryRecoveryTest, CreditOverflowIsReplayableForBothTradeSides) {
            for (const Side taker_side : {Side::Buy, Side::Sell}) {
                SCOPED_TRACE(static_cast<int>(taker_side));
                test::TempTestDirectory directory;
                const auto bootstrap =
                    taker_side == Side::Buy
                        ? TradingBootstrapConfig{{{1, {{1, {maximum, 0}}, {2, {1, 0}}}},
                                                  {2, {{1, {1, 0}}}}}}
                        : TradingBootstrapConfig{
                              {{1, {{1, {1, 0}}, {2, {maximum, 0}}}}, {2, {{2, {1, 0}}}}}};
                const Side maker_side = taker_side == Side::Buy ? Side::Sell : Side::Buy;
                auto runtime =
                    TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
                ASSERT_EQ(submit(*runtime, 1, 2, maker_side, 1, 1).result, TradingResult::Accepted);
                const auto before = snapshot(*runtime);
                const auto rejected = submit(*runtime, 2, 1, taker_side, 1, 1);
                EXPECT_EQ(rejected.result, TradingResult::InvalidOrder);
                EXPECT_TRUE(rejected.events.empty());
                EXPECT_FALSE(rejected.assigned_order_id.has_value());
                expect_state(*runtime, before);
                EXPECT_FALSE(runtime->executor().poisoned());
                ASSERT_EQ(cancel(*runtime, 3, 2, 1).result, TradingResult::Cancelled);
                const auto next = submit(*runtime, 4, 2, maker_side, 1, 1);
                ASSERT_EQ(next.result, TradingResult::Accepted);
                EXPECT_EQ(next.assigned_order_id, 3U);
                const auto after = snapshot(*runtime);
                const auto wal_before = test::read_file_bytes(directory.wal_path());
                runtime.reset();

                std::vector<RecoveredTradingOutcome> outcomes;
                runtime = TradingRuntime::create_durable(instrument, directory.wal_path(),
                                                         bootstrap, &outcomes);
                expect_state(*runtime, after);
                EXPECT_EQ(test::read_file_bytes(directory.wal_path()), wal_before);
                EXPECT_EQ(outcomes,
                          (std::vector<RecoveredTradingOutcome>{{1, TradingResult::Accepted},
                                                                {2, TradingResult::InvalidOrder},
                                                                {3, TradingResult::Cancelled},
                                                                {4, TradingResult::Accepted}}));
                ASSERT_EQ(cancel(*runtime, 5, 2, 3).result, TradingResult::Cancelled);
                const auto resumed = submit(*runtime, 6, 2, maker_side, 1, 1);
                ASSERT_EQ(resumed.result, TradingResult::Accepted);
                EXPECT_EQ(resumed.assigned_order_id, 4U);
                EXPECT_EQ(std::get<OrderAccepted>(resumed.events.front().payload).order.timestamp,
                          4);
            }
        }

        TEST(FinancialBoundaryRecoveryTest, MaximumTotalSelfTradeAndPriceImprovementRemainValid) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig bootstrap{{{1, {{1, {2, 0}}, {2, {maximum, 0}}}}}};
            auto runtime =
                TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            ASSERT_EQ(submit(*runtime, 1, 1, Side::Sell, 1'000, 2).result, TradingResult::Accepted);
            ASSERT_EQ(submit(*runtime, 2, 1, Side::Buy, 2'000, 3).result, TradingResult::Accepted);
            EXPECT_EQ(runtime->order_book().find_order(2)->quantity, 1);
            EXPECT_EQ(runtime->reservations().find(2)->remaining_amount, 4'000);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum - 4'000, 4'000}));
            ASSERT_EQ(cancel(*runtime, 3, 1, 2).result, TradingResult::Cancelled);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*runtime).accounts().find_balance(1, 2),
                      (Balance{maximum, 0}));
            const auto after = snapshot(*runtime);
            runtime.reset();
            runtime = TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            expect_state(*runtime, after);
        }

        TEST(FinancialBoundaryRecoveryTest, ContractPaymentCannotConsumePayeeReleaseCapacity) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig bootstrap{
                {{1, {{2, {1, 0}}}}, {2, {{2, {maximum - 1, 1}}}}}};
            auto runtime =
                TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            ASSERT_TRUE(runtime->agent_registry().register_agent({101, 1}));
            ASSERT_TRUE(runtime->agent_registry().register_agent({202, 2}));
            const auto created = runtime->contract_executor().create_contract(
                {101, 202, {101, 202, 1, ResourceKind::ComputeCredit, 1}});
            ASSERT_EQ(created.result, ContractResult::Success);
            ASSERT_EQ(runtime->contract_executor().accept_contract(1, 202),
                      ContractResult::Success);
            ASSERT_EQ(runtime->contract_executor().fulfill_resource(1, 202),
                      ContractResult::Success);
            const auto before = snapshot(*runtime);
            const auto wal_before = test::read_file_bytes(directory.wal_path());
            EXPECT_EQ(runtime->contract_executor().settle_payment(1, 101),
                      ContractResult::BalanceOverflow);
            expect_state(*runtime, before);
            EXPECT_EQ(runtime->contracts().find(1)->state, ContractState::Fulfilled);
            EXPECT_EQ(test::read_file_bytes(directory.wal_path()), wal_before);
            EXPECT_FALSE(runtime->contract_executor().poisoned());
            runtime.reset();
            runtime = TradingRuntime::create_durable(instrument, directory.wal_path(), bootstrap);
            expect_state(*runtime, before);
            EXPECT_EQ(runtime->contracts().find(1)->state, ContractState::Fulfilled);
        }

        TEST(FinancialBoundaryRecoveryTest,
             ProcessExitAfterRejectionAndCancellationRestartsCleanly) {
            test::TempTestDirectory directory;
            const auto bootstrap = reservation_bootstrap();
            const auto path = directory.wal_path();
            const pid_t child = ::fork();
            ASSERT_NE(child, -1);
            if (child == 0) {
                try {
                    auto runtime = TradingRuntime::create_durable(instrument, path, bootstrap);
                    if (submit(*runtime, 1, 1, Side::Buy, 1'000, 2).result !=
                            TradingResult::Accepted ||
                        submit(*runtime, 2, 1, Side::Sell, 2'000, 1).result !=
                            TradingResult::Accepted ||
                        submit(*runtime, 3, 2, Side::Buy, 2'000, 1).result !=
                            TradingResult::InvalidOrder ||
                        cancel(*runtime, 4, 1, 1).result != TradingResult::Cancelled) {
                        _exit(101);
                    }
                    // No destructors or orderly runtime shutdown in this process.
                    _exit(0);
                } catch (...) {
                    _exit(102);
                }
            }
            int child_status = 0;
            pid_t waited;
            do {
                waited = ::waitpid(child, &child_status, 0);
            } while (waited == -1 && errno == EINTR);
            ASSERT_EQ(waited, child);
            ASSERT_TRUE(WIFEXITED(child_status));
            ASSERT_EQ(WEXITSTATUS(child_status), 0);

            std::vector<RecoveredTradingOutcome> outcomes;
            auto recovered = TradingRuntime::create_durable(instrument, path, bootstrap, &outcomes);
            ASSERT_EQ(outcomes.size(), 4U);
            EXPECT_EQ(outcomes[2].result, TradingResult::InvalidOrder);
            EXPECT_EQ(outcomes[3].result, TradingResult::Cancelled);
            EXPECT_FALSE(recovered->order_book().find_order(1).has_value());
            EXPECT_FALSE(recovered->reservations().find(1).has_value());
            ASSERT_TRUE(recovered->order_book().find_order(2).has_value());
            EXPECT_EQ(recovered->order_book().find_order(2)->quantity, 1);
            EXPECT_EQ(static_cast<const TradingRuntime &>(*recovered).accounts().find_balance(1, 2),
                      (Balance{maximum, 0}));
            EXPECT_EQ(recovered->ledger().entries().size(), 3U);
            const auto next = submit(*recovered, 5, 2, Side::Buy, 1'000, 1);
            EXPECT_EQ(next.result, TradingResult::Accepted);
            EXPECT_EQ(next.assigned_order_id, 4U);
        }
    } // namespace
} // namespace exchange
