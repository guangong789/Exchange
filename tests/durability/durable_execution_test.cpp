#include "durability/command_journal.hpp"
#include "durability/execution_wal_writer.hpp"
#include "execution/trading_request_executor.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};

        TradingBootstrapConfig funded_bootstrap() {
            return TradingBootstrapConfig{{BootstrapAccount{
                1,
                {{instrument.quote_asset, {1'000, 0}}}}}};
        }

        TradingBootstrapConfig empty_account_bootstrap() {
            return TradingBootstrapConfig{{BootstrapAccount{1, {}}}};
        }

        TradingRequest submit_request(
            RequestId request_id,
            AccountId account_id,
            Price price = 100,
            Quantity quantity = 1) {
            return TradingRequest{
                request_id,
                account_id,
                SubmitTradingRequest{Side::Buy, price, quantity}};
        }

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern = "/tmp/exchange-durable-test-XXXXXX";
                path_ = ::mkdtemp(pattern.data());
                if (path_.empty()) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "mkdtemp");
                }
            }

            ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] std::string wal_path() const {
                return path_ + "/execution.wal";
            }

        private:
            std::string path_;
        };

        class RecordingJournal final : public ExecutionCommandJournal {
        public:
            enum class Failure {
                None,
                Write,
                Sync,
            };

            void append(const ExecutionCommand& command) override {
                if (before_append) {
                    before_append();
                }
                ++attempts;
                if (failure == Failure::Write) {
                    throw std::runtime_error("injected WAL write failure");
                }
                commands.push_back(command);
                if (failure == Failure::Sync) {
                    throw std::runtime_error("injected WAL sync failure");
                }
            }

            Failure failure{Failure::None};
            std::function<void()> before_append;
            std::size_t attempts{};
            std::vector<ExecutionCommand> commands;
        };

        struct ExecutionWorld {
            explicit ExecutionWorld(ExecutionCommandJournal* journal)
                : executor(
                      instrument,
                      coordinator,
                      events,
                      sequencer,
                      journal) {}

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
            ExecutionSequencer sequencer;
            TradingRequestExecutor executor;
        };

        TEST(DurableExecutionTest, DurabilityCompletesBeforeApplication) {
            RecordingJournal journal;
            ExecutionWorld world{&journal};
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);
            journal.before_append = [&] {
                EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
                EXPECT_TRUE(world.reservations.find(1) == std::nullopt);
                EXPECT_TRUE(world.ledger.entries().empty());
            };

            const TradingResponse response = world.executor.execute(
                submit_request(1, 1));

            EXPECT_EQ(response.result, TradingResult::Accepted);
            ASSERT_EQ(journal.commands.size(), 1U);
            EXPECT_TRUE(world.matching_engine.order_book().find_order(1).has_value());
        }

        TEST(DurableExecutionTest, WriteFailurePreventsApplyAndPoisonsExecutor) {
            RecordingJournal journal;
            journal.failure = RecordingJournal::Failure::Write;
            ExecutionWorld world{&journal};
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);

            EXPECT_THROW(
                static_cast<void>(world.executor.execute(
                    submit_request(1, 1))),
                std::runtime_error);

            EXPECT_TRUE(world.executor.poisoned());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            EXPECT_FALSE(world.reservations.find(1).has_value());
            EXPECT_TRUE(world.ledger.entries().empty());
            journal.failure = RecordingJournal::Failure::None;
            EXPECT_THROW(
                static_cast<void>(world.executor.execute(
                    submit_request(2, 1))),
                std::logic_error);
            EXPECT_EQ(journal.attempts, 1U);
        }

        TEST(DurableExecutionTest, SyncFailurePreventsApplyAndPoisonsExecutor) {
            RecordingJournal journal;
            journal.failure = RecordingJournal::Failure::Sync;
            ExecutionWorld world{&journal};
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);

            EXPECT_THROW(
                static_cast<void>(world.executor.execute(
                    submit_request(1, 1))),
                std::runtime_error);

            EXPECT_TRUE(world.executor.poisoned());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            EXPECT_EQ(
                world.accounts.find_balance(1, 10),
                (Balance{1'000, 0}));
            EXPECT_TRUE(world.ledger.entries().empty());
        }

        TEST(DurableExecutionTest, PreAdmissionRejectionIsNotJournaled) {
            RecordingJournal journal;
            ExecutionWorld world{&journal};

            const TradingResponse response = world.executor.execute(
                submit_request(1, 1, 0, 1));

            EXPECT_EQ(response.result, TradingResult::InvalidOrder);
            EXPECT_TRUE(journal.commands.empty());
            EXPECT_FALSE(world.executor.poisoned());
        }

        TEST(DurableExecutionTest,
             BusinessRejectionIsJournaledAndPreservesBothSequenceGaps) {
            RecordingJournal journal;
            ExecutionWorld world{&journal};

            const TradingResponse rejected = world.executor.execute(
                submit_request(1, 9));
            EXPECT_EQ(rejected.result, TradingResult::AccountNotFound);
            ASSERT_EQ(journal.commands.size(), 1U);
            const auto& rejected_command = std::get<SubmitExecutionCommand>(
                journal.commands[0]);
            EXPECT_EQ(rejected_command.order.id, 1U);

            ASSERT_TRUE(world.accounts.create_account(9));
            world.accounts.fund(9, 10, 1'000);
            const TradingResponse accepted = world.executor.execute(
                submit_request(2, 9));
            EXPECT_EQ(accepted.result, TradingResult::Accepted);
            EXPECT_EQ(accepted.assigned_order_id, 2U);
            ASSERT_EQ(journal.commands.size(), 2U);
            EXPECT_EQ(
                std::get<SubmitExecutionCommand>(journal.commands[1]).order.id,
                2U);
        }

        TEST(DurableExecutionTest,
             UnexpectedApplyFailureLeavesRecordDurableAndPoisonsExecutor) {
            TemporaryDirectory directory;
            ExecutionWalWriter writer{
                directory.wal_path(),
                instrument,
                calculate_bootstrap_fingerprint(TradingBootstrapConfig{})};
            ExecutionWorld world{&writer};
            ASSERT_TRUE(world.accounts.create_account(1));
            world.accounts.fund(1, 10, 1'000);
            ASSERT_EQ(
                world.accounts.reserve(1, 10, 100),
                ReserveResult::Success);
            ASSERT_TRUE(world.reservations.create(44, 1, 10, 100));

            EXPECT_THROW(
                static_cast<void>(world.executor.execute(TradingRequest{
                    7,
                    1,
                    CancelTradingRequest{44}})),
                std::logic_error);

            EXPECT_TRUE(world.executor.poisoned());
            EXPECT_EQ(writer.record_count(), 1U);
            EXPECT_THROW(
                static_cast<void>(world.executor.execute(
                    submit_request(8, 1))),
                std::logic_error);
        }

        TEST(DurableTradingRuntimeTest, LogsThenAppliesLiveCommand) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        funded_bootstrap());

                const TradingResponse response = runtime->executor().execute(
                    submit_request(1, 1));
                EXPECT_EQ(response.result, TradingResult::Accepted);
                EXPECT_TRUE(runtime->order_book().find_order(1).has_value());
            }

            ExecutionWalWriter writer{
                path,
                instrument,
                calculate_bootstrap_fingerprint(funded_bootstrap())};
            EXPECT_EQ(writer.record_count(), 1U);
            EXPECT_EQ(writer.next_sequence(), 2U);
        }

        TEST(DurableTradingRuntimeTest,
             RejectsBootstrapMutationAfterRecoverySealsRuntime) {
            TemporaryDirectory directory;
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    directory.wal_path(),
                    empty_account_bootstrap());
            EXPECT_THROW(
                static_cast<void>(runtime->accounts()),
                std::logic_error);
        }

        TEST(DurableTradingRuntimeTest,
             RecoversHistoricalCommandsBeforeReturningRuntime) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path,
                    instrument,
                    calculate_bootstrap_fingerprint(funded_bootstrap())};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 1}});
            }

            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    funded_bootstrap());
            EXPECT_TRUE(runtime->order_book().find_order(1).has_value());
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().find_balance(1, 10),
                (Balance{900, 100}));
        }
    }  // namespace
}  // namespace exchange
