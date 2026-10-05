#include "durability/execution_recovery.hpp"
#include "durability/execution_wal_writer.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr Amount initial_funds = 10'000;

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern = "/tmp/exchange-recovery-test-XXXXXX";
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

        TradingBootstrapConfig bootstrap_config() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{instrument.base_asset, {initial_funds, 0}},
                     {instrument.quote_asset, {initial_funds, 0}}}},
                BootstrapAccount{
                    2,
                    {{instrument.base_asset, {initial_funds, 0}},
                     {instrument.quote_asset, {initial_funds, 0}}}},
                BootstrapAccount{3, {}},
            }};
        }

        BootstrapFingerprint bootstrap_fingerprint() {
            return calculate_bootstrap_fingerprint(bootstrap_config());
        }

        TradingRequest submit(
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

        TradingRequest cancel(
            RequestId request_id,
            AccountId account_id,
            OrderId order_id) {
            return TradingRequest{
                request_id,
                account_id,
                CancelTradingRequest{order_id}};
        }

        WalBytes read_file(const std::string& path) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "open recovery test WAL");
            }
            struct stat status {};
            if (::fstat(fd, &status) == -1) {
                const int saved_errno = errno;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "stat recovery test WAL");
            }
            WalBytes bytes(static_cast<std::size_t>(status.st_size));
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const ssize_t count = ::read(
                    fd,
                    bytes.data() + offset,
                    bytes.size() - offset);
                if (count > 0) {
                    offset += static_cast<std::size_t>(count);
                    continue;
                }
                if (count == -1 && errno == EINTR) {
                    continue;
                }
                const int saved_errno = count == -1 ? errno : EIO;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "read recovery test WAL");
            }
            ::close(fd);
            return bytes;
        }

        void append_raw(const std::string& path, const WalBytes& bytes) {
            const int fd = ::open(
                path.c_str(),
                O_WRONLY | O_APPEND | O_CLOEXEC);
            if (fd == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "open recovery test WAL append");
            }
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const ssize_t count = ::write(
                    fd,
                    bytes.data() + offset,
                    bytes.size() - offset);
                if (count > 0) {
                    offset += static_cast<std::size_t>(count);
                    continue;
                }
                if (count == -1 && errno == EINTR) {
                    continue;
                }
                const int saved_errno = count == -1 ? errno : EIO;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "append recovery test WAL");
            }
            if (::fdatasync(fd) == -1) {
                const int saved_errno = errno;
                ::close(fd);
                throw std::system_error(
                    saved_errno,
                    std::generic_category(),
                    "sync recovery test WAL");
            }
            ::close(fd);
        }

        std::unique_ptr<TradingRuntime> open_runtime(
            const std::string& path,
            std::vector<RecoveredTradingOutcome>* outcomes = nullptr);

        std::uint32_t test_crc32c(const WalBytes& bytes) {
            constexpr std::uint32_t polynomial = 0x82F63B78U;
            std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
            for (std::size_t index = 0; index < bytes.size() - 4; ++index) {
                crc ^= bytes[index];
                for (int bit = 0; bit < 8; ++bit) {
                    const std::uint32_t mask =
                        0U - static_cast<std::uint32_t>(crc & 1U);
                    crc = (crc >> 1U) ^ (polynomial & mask);
                }
            }
            return ~crc;
        }

        void rewrite_checksum(WalBytes& bytes) {
            const std::uint32_t checksum = test_crc32c(bytes);
            for (std::size_t index = 0; index < 4; ++index) {
                bytes[bytes.size() - 4 + index] =
                    static_cast<std::uint8_t>(checksum >> (index * 8));
            }
        }

        void expect_runtime_scan_failure(
            const std::function<void(WalBytes&)>& mutate,
            WalError expected_error) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path, instrument, bootstrap_fingerprint()};
            }
            const WalRecordEncodeResult encoded = encode_wal_record(
                WalRecord{1, CancelExecutionCommand{1, 1, 1}},
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            WalBytes record = std::get<WalBytes>(encoded);
            mutate(record);
            append_raw(path, record);

            try {
                static_cast<void>(open_runtime(path));
                FAIL() << "corrupt WAL was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), expected_error);
            }
        }

        std::unique_ptr<TradingRuntime> open_runtime(
            const std::string& path,
            std::vector<RecoveredTradingOutcome>* outcomes) {
            return TradingRuntime::create_durable(
                instrument,
                path,
                bootstrap_config(),
                outcomes);
        }

        void expect_bootstrap_mismatch(
            const std::string& path,
            const TradingBootstrapConfig& changed_bootstrap) {
            try {
                static_cast<void>(TradingRuntime::create_durable(
                    instrument, path, changed_bootstrap));
                FAIL() << "bootstrap mismatch was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::BootstrapMismatch);
            }
        }

        struct StateSnapshot {
            AccountStore::AccountBalances balances;
            std::vector<LedgerEntry> ledger;
            std::map<OrderId, OrderReservation> reservations;
            std::map<OrderId, std::optional<Order>> orders;
            std::vector<std::optional<Contract>> contracts;
            bool trading_poisoned{};
            bool contracts_poisoned{};
            std::optional<OrderReservation> reservation;
            std::optional<Order> resting_order;
            std::size_t order_count{};
            std::optional<Price> best_bid;
            std::optional<Price> best_ask;
        };

        StateSnapshot snapshot(TradingRuntime& runtime) {
            StateSnapshot result;
            result.balances = std::as_const(runtime).accounts().entries();
            result.reservations = runtime.reservations().entries();
            for (const auto& [order_id, reservation] : result.reservations) {
                static_cast<void>(reservation);
                result.orders.emplace(
                    order_id, runtime.order_book().find_order(order_id));
            }
            for (ContractId id = 1; id <= runtime.contracts().size(); ++id) {
                result.contracts.push_back(runtime.contracts().find(id));
            }
            result.ledger = runtime.ledger().entries();
            result.reservation = runtime.reservations().find(7);
            result.resting_order = runtime.order_book().find_order(7);
            result.order_count = runtime.order_book().order_count();
            result.best_bid = runtime.order_book().best_bid();
            result.best_ask = runtime.order_book().best_ask();
            result.trading_poisoned = runtime.executor().poisoned();
            result.contracts_poisoned = runtime.contract_executor().poisoned();
            return result;
        }

        void expect_same_order(
            const std::optional<Order>& left,
            const std::optional<Order>& right) {
            ASSERT_EQ(left.has_value(), right.has_value());
            if (!left.has_value()) {
                return;
            }
            EXPECT_EQ(left->id, right->id);
            EXPECT_EQ(left->side, right->side);
            EXPECT_EQ(left->type, right->type);
            EXPECT_EQ(left->price, right->price);
            EXPECT_EQ(left->quantity, right->quantity);
            EXPECT_EQ(left->timestamp, right->timestamp);
        }

        void expect_same_state(
            const StateSnapshot& left,
            const StateSnapshot& right) {
            EXPECT_EQ(left.balances, right.balances);
            EXPECT_EQ(left.ledger, right.ledger);
            EXPECT_EQ(left.reservations, right.reservations);
            ASSERT_EQ(left.orders.size(), right.orders.size());
            for (const auto& [order_id, order] : left.orders) {
                ASSERT_TRUE(right.orders.contains(order_id));
                expect_same_order(order, right.orders.at(order_id));
            }
            EXPECT_EQ(left.contracts, right.contracts);
            EXPECT_EQ(left.trading_poisoned, right.trading_poisoned);
            EXPECT_EQ(left.contracts_poisoned, right.contracts_poisoned);
            EXPECT_EQ(left.reservation, right.reservation);
            expect_same_order(left.resting_order, right.resting_order);
            EXPECT_EQ(left.order_count, right.order_count);
            EXPECT_EQ(left.best_bid, right.best_bid);
            EXPECT_EQ(left.best_ask, right.best_ask);
        }

        void execute_recovery_workload(TradingRuntime& runtime) {
            EXPECT_EQ(runtime.executor().execute(
                submit(1, 1, Side::Sell, 100, 5)).result,
                TradingResult::Accepted);
            EXPECT_EQ(runtime.executor().execute(
                submit(2, 2, Side::Buy, 100, 2)).result,
                TradingResult::Accepted);
            EXPECT_EQ(runtime.executor().execute(
                submit(3, 2, Side::Buy, 100, 3)).result,
                TradingResult::Accepted);
            EXPECT_EQ(runtime.executor().execute(
                submit(4, 2, Side::Buy, 90, 2)).result,
                TradingResult::Accepted);
            EXPECT_EQ(runtime.executor().execute(cancel(5, 2, 4)).result,
                TradingResult::Cancelled);
            EXPECT_EQ(runtime.executor().execute(cancel(6, 2, 999)).result,
                TradingResult::CancelNotFound);
            EXPECT_EQ(runtime.executor().execute(
                submit(7, 3, Side::Buy, 100, 1)).result,
                TradingResult::InsufficientFunds);
            EXPECT_EQ(runtime.executor().execute(
                submit(8, 99, Side::Buy, 100, 1)).result,
                TradingResult::AccountNotFound);
            EXPECT_EQ(runtime.executor().execute(
                submit(9, 1, Side::Sell, 110, 2)).result,
                TradingResult::Accepted);
            EXPECT_EQ(runtime.executor().execute(
                submit(10, 2, Side::Buy, 110, 1)).result,
                TradingResult::Accepted);
        }

        TEST(ExecutionRecoveryOutcomeTest,
             CollectsTradingResultsInWalOrderAndReplacesOutput) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                const auto runtime = open_runtime(path);
                execute_recovery_workload(*runtime);
                ASSERT_EQ(runtime->executor().execute(cancel(11, 2, 7)).result,
                          TradingResult::CancelNotOwner);
                ASSERT_EQ(runtime->executor().execute(cancel(12, 1, 7)).result,
                          TradingResult::Cancelled);
                ASSERT_EQ(runtime->executor().execute(cancel(13, 1, 7)).result,
                          TradingResult::CancelNotFound);
            }

            std::vector<RecoveredTradingOutcome> outcomes{
                {999, TradingResult::InvalidRequest}};
            const auto recovered = open_runtime(path, &outcomes);
            const std::vector<RecoveredTradingOutcome> expected{
                {1, TradingResult::Accepted},
                {2, TradingResult::Accepted},
                {3, TradingResult::Accepted},
                {4, TradingResult::Accepted},
                {5, TradingResult::Cancelled},
                {6, TradingResult::CancelNotFound},
                {7, TradingResult::InsufficientFunds},
                {8, TradingResult::AccountNotFound},
                {9, TradingResult::Accepted},
                {10, TradingResult::Accepted},
                {11, TradingResult::CancelNotOwner},
                {12, TradingResult::Cancelled},
                {13, TradingResult::CancelNotFound}};
            EXPECT_EQ(outcomes, expected);
            for (std::size_t index = 1; index < outcomes.size(); ++index) {
                EXPECT_LT(outcomes[index - 1].wal_sequence,
                          outcomes[index].wal_sequence);
            }
            EXPECT_FALSE(recovered->executor().poisoned());
            EXPECT_FALSE(recovered->contract_executor().poisoned());

            ASSERT_EQ(recovered->executor().execute(
                submit(14, 1, Side::Buy, 80, 1)).result,
                TradingResult::Accepted);
            EXPECT_EQ(outcomes, expected);
        }

        TEST(ExecutionRecoveryOutcomeTest,
             RepeatedRequestIdsRemainSeparateWalOutcomes) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                const auto runtime = open_runtime(path);
                ASSERT_EQ(runtime->executor().execute(
                    submit(42, 1, Side::Buy, 90, 1)).result,
                    TradingResult::Accepted);
                ASSERT_EQ(runtime->executor().execute(
                    submit(42, 3, Side::Buy, 100, 1)).result,
                    TradingResult::InsufficientFunds);
                ASSERT_EQ(runtime->executor().execute(cancel(42, 2, 1)).result,
                          TradingResult::CancelNotOwner);
            }

            std::vector<RecoveredTradingOutcome> outcomes;
            const auto recovered = open_runtime(path, &outcomes);
            EXPECT_EQ(outcomes, (std::vector<RecoveredTradingOutcome>{
                {1, TradingResult::Accepted},
                {2, TradingResult::InsufficientFunds},
                {3, TradingResult::CancelNotOwner}}));
            const auto scan = scan_execution_wal(
                read_file(path), instrument, bootstrap_fingerprint());
            ASSERT_EQ(scan.records.size(), 3U);
            EXPECT_EQ(std::get<SubmitExecutionCommand>(
                scan.records[0].command).request_id, 42U);
            EXPECT_EQ(std::get<SubmitExecutionCommand>(
                scan.records[1].command).request_id, 42U);
            EXPECT_EQ(std::get<CancelExecutionCommand>(
                scan.records[2].command).request_id, 42U);
        }

        TEST(ExecutionRecoveryOutcomeTest, EmptyRecoveryReplacesOutputWithEmptyCollection) {
            TemporaryDirectory directory;
            std::vector<RecoveredTradingOutcome> outcomes{
                {999, TradingResult::InvalidRequest}};
            const auto recovered = open_runtime(directory.wal_path(), &outcomes);
            EXPECT_TRUE(outcomes.empty());
            EXPECT_EQ(recovered->order_book().order_count(), 0U);
        }

        TEST(ExecutionRecoveryOutcomeTest,
             ObservationPreservesCompleteMixedTradingAndContractState) {
            TemporaryDirectory source_directory;
            TemporaryDirectory normal_directory;
            TemporaryDirectory observed_directory;
            const auto register_agents = [](TradingRuntime& runtime) {
                ASSERT_TRUE(runtime.agent_registry().register_agent({101, 1}));
                ASSERT_TRUE(runtime.agent_registry().register_agent({202, 2}));
            };
            const CreateContractRequest contract_request{
                101, 202, {101, 202, 500, ResourceKind::ComputeCredit, 20}};
            StateSnapshot original;
            {
                const auto runtime = open_runtime(source_directory.wal_path());
                execute_recovery_workload(*runtime);
                register_agents(*runtime);
                ASSERT_EQ(runtime->contract_executor().create_contract(
                    contract_request).contract_id, 1U);
                ASSERT_EQ(runtime->contract_executor().accept_contract(1, 202),
                          ContractResult::Success);
                ASSERT_EQ(runtime->contract_executor().fulfill_resource(1, 202),
                          ContractResult::Success);
                ASSERT_EQ(runtime->contract_executor().settle_payment(1, 101),
                          ContractResult::Success);
                ASSERT_EQ(runtime->contract_executor().create_contract(
                    contract_request).contract_id, 2U);
                ASSERT_EQ(runtime->executor().execute(
                    submit(11, 2, Side::Buy, 80, 1)).result,
                    TradingResult::Accepted);
                original = snapshot(*runtime);
                ASSERT_EQ(original.orders.size(), 2U);
                ASSERT_EQ(original.contracts.size(), 2U);
            }
            const WalBytes original_wal = read_file(source_directory.wal_path());
            std::filesystem::copy_file(source_directory.wal_path(),
                                       normal_directory.wal_path());
            std::filesystem::copy_file(source_directory.wal_path(),
                                       observed_directory.wal_path());

            const auto normal = open_runtime(normal_directory.wal_path());
            std::vector<RecoveredTradingOutcome> outcomes;
            const auto observed = open_runtime(observed_directory.wal_path(), &outcomes);
            expect_same_state(original, snapshot(*normal));
            expect_same_state(snapshot(*normal), snapshot(*observed));
            EXPECT_EQ(read_file(normal_directory.wal_path()), original_wal);
            EXPECT_EQ(read_file(observed_directory.wal_path()), original_wal);
            ASSERT_EQ(outcomes.size(), 11U);
            EXPECT_EQ(outcomes.back(), (RecoveredTradingOutcome{
                16, TradingResult::Accepted}));
            for (const auto& outcome : outcomes) {
                EXPECT_TRUE(outcome.wal_sequence <= 10 || outcome.wal_sequence == 16);
            }

            // Production probes verify the otherwise private order/timestamp,
            // contract and Ledger sequencers as well as subsequent WAL append.
            for (TradingRuntime* runtime : {normal.get(), observed.get()}) {
                const auto next = runtime->executor().execute(
                    submit(12, 2, Side::Buy, 70, 1));
                ASSERT_EQ(next.result, TradingResult::Accepted);
                EXPECT_EQ(next.assigned_order_id, 10U);
                ASSERT_EQ(next.events.size(), 1U);
                EXPECT_EQ(std::get<OrderAccepted>(next.events[0].payload)
                              .order.timestamp, 10);
                register_agents(*runtime);
                const auto next_contract = runtime->contract_executor()
                    .create_contract(contract_request);
                EXPECT_EQ(next_contract.result, ContractResult::Success);
                EXPECT_EQ(next_contract.contract_id, 3U);
            }
            expect_same_state(snapshot(*normal), snapshot(*observed));
            EXPECT_EQ(read_file(normal_directory.wal_path()),
                      read_file(observed_directory.wal_path()));
            const auto scan = scan_execution_wal(read_file(observed_directory.wal_path()),
                                                instrument, bootstrap_fingerprint());
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 18U);
            EXPECT_EQ(scan.records.back().sequence, 18U);
            EXPECT_EQ(outcomes.size(), 11U);
        }

        struct RecoveryState {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching{events};
            Ledger ledger;
            ExecutionSequencer sequencer;
            ExecutionCoordinator coordinator{
                instrument, accounts, reservations, matching, events, ledger};
            TradingCommandApplier applier{coordinator, events};
            ContractStore contracts;
            ContractSequencer contract_sequencer;
            ContractCommandApplier contract_applier{
                contracts, accounts, ledger, instrument.quote_asset};
            ExecutionRecovery recovery{
                instrument, accounts, reservations, matching, events, ledger,
                sequencer, applier, contracts, contract_sequencer, contract_applier};

            explicit RecoveryState(InstrumentContext execution_instrument = instrument)
                : coordinator{execution_instrument, accounts, reservations,
                              matching, events, ledger} {
                apply_trading_bootstrap(bootstrap_config(), accounts);
            }
        };

        TEST(ExecutionRecoveryOutcomeTest, DirectRecoveryPublishesAfterChecksAndReturnsSameSummary) {
            const std::vector<WalRecord> records{
                {1, SubmitExecutionCommand{
                    42, 1, {1, Side::Buy, OrderType::Limit, 100, 1, 1}}},
                {2, CancelExecutionCommand{42, 1, 1}}};
            RecoveryState normal;
            RecoveryState observed;
            std::vector<RecoveredTradingOutcome> outcomes;
            const auto normal_summary = normal.recovery.recover(records, 2, 3);
            const auto observed_summary = observed.recovery.recover(records, 2, 3, &outcomes);
            EXPECT_EQ(observed_summary.records_replayed, normal_summary.records_replayed);
            EXPECT_EQ(observed_summary.submit_attempts, normal_summary.submit_attempts);
            EXPECT_EQ(observed_summary.next_execution_identity,
                      normal_summary.next_execution_identity);
            EXPECT_EQ(observed_summary.next_contract_id, normal_summary.next_contract_id);
            EXPECT_EQ(observed_summary.next_execution_identity, (AssignedOrderIdentity{2, 2}));
            EXPECT_EQ(outcomes, (std::vector<RecoveredTradingOutcome>{
                {1, TradingResult::Accepted}, {2, TradingResult::Cancelled}}));
            EXPECT_EQ(observed.accounts.entries(), normal.accounts.entries());
            EXPECT_EQ(observed.reservations.entries(), normal.reservations.entries());
            EXPECT_EQ(observed.ledger.entries(), normal.ledger.entries());
            EXPECT_EQ(observed.matching.order_book().order_count(), 0U);
            EXPECT_TRUE(observed.events.events().empty());
        }

        TEST(ExecutionRecoveryOutcomeTest, SequenceFailureAfterReplayLeavesOutputUnchanged) {
            const std::vector<WalRecord> records{
                {1, SubmitExecutionCommand{
                    1, 1, {1, Side::Buy, OrderType::Limit, 100, 1, 1}}},
                {3, CancelExecutionCommand{2, 1, 1}}};
            const std::vector<RecoveredTradingOutcome> sentinel{
                {999, TradingResult::InvalidRequest}};
            for (const bool collect : {false, true}) {
                RecoveryState state;
                auto outcomes = sentinel;
                try {
                    static_cast<void>(state.recovery.recover(
                        records, 2, 4, collect ? &outcomes : nullptr));
                    FAIL() << "sequence gap was accepted";
                } catch (const ExecutionRecoveryException& error) {
                    EXPECT_EQ(error.failure(), ExecutionRecoveryFailure::WalPrefixMismatch);
                    EXPECT_EQ(error.wal_sequence(), 3U);
                }
                EXPECT_TRUE(state.matching.order_book().find_order(1));
                EXPECT_EQ(outcomes, sentinel);
            }
        }

        TEST(ExecutionRecoveryOutcomeTest, FinalInvariantFailureLeavesOutputUnchanged) {
            const std::vector<WalRecord> records{
                {1, SubmitExecutionCommand{
                    1, 1, {1, Side::Sell, OrderType::Limit, 100, 1, 1}}}};
            const std::vector<RecoveredTradingOutcome> sentinel{
                {999, TradingResult::InvalidRequest}};
            for (const bool collect : {false, true}) {
                // Deliberately miswire the fixture's applier instrument so a
                // completed replay violates recovery's reservation invariant.
                RecoveryState state{InstrumentContext{30, 10, 1, 1, 1}};
                state.accounts.fund(1, 30, initial_funds);
                auto outcomes = sentinel;
                try {
                    static_cast<void>(state.recovery.recover(
                        records, 1, 2, collect ? &outcomes : nullptr));
                    FAIL() << "invalid recovered reservation was accepted";
                } catch (const ExecutionRecoveryException& error) {
                    EXPECT_EQ(error.failure(), ExecutionRecoveryFailure::ReservationInvariant);
                    EXPECT_EQ(error.wal_sequence(), 1U);
                }
                EXPECT_TRUE(state.matching.order_book().find_order(1));
                EXPECT_EQ(outcomes, sentinel);
            }
        }

        TEST(ExecutionRecoveryOutcomeTest, TornTailObservationPreservesRepairAndAppendPolicy) {
            TemporaryDirectory normal_directory;
            TemporaryDirectory observed_directory;
            const auto normal_path = normal_directory.wal_path();
            const auto observed_path = observed_directory.wal_path();
            {
                const auto runtime = open_runtime(normal_path);
                ASSERT_EQ(runtime->executor().execute(
                    submit(1, 1, Side::Buy, 100, 1)).result,
                    TradingResult::Accepted);
            }
            const auto encoded = encode_wal_record(
                {2, CancelExecutionCommand{2, 1, 1}}, instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            auto partial = std::get<WalBytes>(encoded);
            partial.pop_back();
            append_raw(normal_path, partial);
            std::filesystem::copy_file(normal_path, observed_path);

            const auto normal = open_runtime(normal_path);
            std::vector<RecoveredTradingOutcome> outcomes;
            const auto observed = open_runtime(observed_path, &outcomes);
            EXPECT_EQ(outcomes, (std::vector<RecoveredTradingOutcome>{
                {1, TradingResult::Accepted}}));
            expect_same_state(snapshot(*normal), snapshot(*observed));
            EXPECT_EQ(read_file(normal_path), read_file(observed_path));
            EXPECT_EQ(normal->executor().execute(cancel(2, 1, 1)).result,
                      TradingResult::Cancelled);
            EXPECT_EQ(observed->executor().execute(cancel(2, 1, 1)).result,
                      TradingResult::Cancelled);
            EXPECT_EQ(read_file(normal_path), read_file(observed_path));
            const auto scan = scan_execution_wal(read_file(observed_path),
                                                instrument, bootstrap_fingerprint());
            EXPECT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 2U);
            EXPECT_EQ(scan.records.back().sequence, 2U);
            EXPECT_EQ(outcomes.size(), 1U);
        }

        TEST(ExecutionRecoveryOutcomeTest, UnexpectedApplyFailureDoesNotPublishEarlierOutcome) {
            TemporaryDirectory directory;
            const auto path = directory.wal_path();
            const TradingBootstrapConfig overflowing_bootstrap{{
                BootstrapAccount{1, {{20, {1, 0}}}},
                BootstrapAccount{2, {{10, {100, 0}},
                    {20, {std::numeric_limits<Amount>::max(), 0}}}}}};
            {
                ExecutionWalWriter writer{path, instrument,
                    calculate_bootstrap_fingerprint(overflowing_bootstrap)};
                writer.append(SubmitExecutionCommand{
                    1, 1, {1, Side::Sell, OrderType::Limit, 100, 1, 1}});
                writer.append(SubmitExecutionCommand{
                    2, 2, {2, Side::Buy, OrderType::Limit, 100, 1, 2}});
            }
            const auto original_wal = read_file(path);
            const std::vector<RecoveredTradingOutcome> sentinel{
                {999, TradingResult::InvalidRequest}};
            for (const bool collect : {false, true}) {
                auto outcomes = sentinel;
                try {
                    static_cast<void>(TradingRuntime::create_durable(
                        instrument, path, overflowing_bootstrap,
                        collect ? &outcomes : nullptr));
                    FAIL() << "overflowing replay was accepted";
                } catch (const ExecutionRecoveryException& error) {
                    EXPECT_EQ(error.failure(), ExecutionRecoveryFailure::CommandApplication);
                    EXPECT_EQ(error.wal_sequence(), 2U);
                }
                EXPECT_EQ(outcomes, sentinel);
                EXPECT_EQ(read_file(path), original_wal);
            }
        }

        TEST(ExecutionRecoveryOutcomeTest, BootstrapMismatchLeavesOutputUnchanged) {
            TemporaryDirectory directory;
            const auto path = directory.wal_path();
            { const auto runtime = open_runtime(path); }
            auto bootstrap = bootstrap_config();
            ++bootstrap.accounts[0].balances[0].balance.available;
            const std::vector<RecoveredTradingOutcome> sentinel{
                {999, TradingResult::InvalidRequest}};
            auto outcomes = sentinel;
            try {
                static_cast<void>(TradingRuntime::create_durable(
                    instrument, path, bootstrap, &outcomes));
                FAIL() << "bootstrap mismatch was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::BootstrapMismatch);
            }
            EXPECT_EQ(outcomes, sentinel);
        }

        TEST(ExecutionRecoveryOutcomeTest, CorruptWalLeavesOutputUnchanged) {
            TemporaryDirectory directory;
            const auto path = directory.wal_path();
            { const auto runtime = open_runtime(path); }
            const auto encoded = encode_wal_record(
                {1, CancelExecutionCommand{1, 1, 1}}, instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            auto bytes = std::get<WalBytes>(encoded);
            bytes[20] ^= 0x80U;
            append_raw(path, bytes);
            const std::vector<RecoveredTradingOutcome> sentinel{
                {999, TradingResult::InvalidRequest}};
            auto outcomes = sentinel;
            try {
                static_cast<void>(open_runtime(path, &outcomes));
                FAIL() << "corrupt WAL was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.failure(), ExecutionWalWriterFailure::Scan);
                EXPECT_EQ(error.wal_error(), WalError::ChecksumMismatch);
            }
            EXPECT_EQ(outcomes, sentinel);
        }

        TEST(ExecutionRecoveryTest,
             RestartReconstructsTradesFillsCancelRejectionsAndState) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            StateSnapshot before;
            {
                std::unique_ptr<TradingRuntime> runtime = open_runtime(path);
                execute_recovery_workload(*runtime);
                before = snapshot(*runtime);
                ASSERT_TRUE(before.resting_order.has_value());
                EXPECT_EQ(before.resting_order->quantity, 1);
            }

            std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
            const StateSnapshot after = snapshot(*recovered);
            expect_same_state(before, after);

            const std::size_t recovered_ledger_size =
                recovered->ledger().entries().size();
            const TradingResponse next = recovered->executor().execute(
                submit(11, 2, Side::Buy, 80, 1));
            ASSERT_EQ(next.result, TradingResult::Accepted);
            EXPECT_EQ(next.assigned_order_id, 9U);
            ASSERT_EQ(next.events.size(), 1U);
            const auto& accepted = std::get<OrderAccepted>(
                next.events.front().payload);
            EXPECT_EQ(accepted.order.timestamp, 9);
            ASSERT_EQ(
                recovered->ledger().entries().size(),
                recovered_ledger_size + 1);
            EXPECT_EQ(
                recovered->ledger().entries().back().sequence,
                recovered_ledger_size + 1);

            const WalScanResult scan = scan_execution_wal(
                read_file(path),
                instrument,
                bootstrap_fingerprint());
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 11U);
            EXPECT_EQ(scan.records.back().sequence, 11U);
        }

        TEST(ExecutionRecoveryTest,
             SameWalRecoversEquivalentIndependentFreshRuntimes) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime = open_runtime(path);
                execute_recovery_workload(*runtime);
            }

            StateSnapshot first;
            {
                std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
                first = snapshot(*recovered);
            }
            std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
            expect_same_state(first, snapshot(*recovered));
        }

        TEST(ExecutionRecoveryTest,
             EquivalentBootstrapConstructionOrderRecoversSameWal) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime = open_runtime(path);
                ASSERT_EQ(runtime->executor().execute(
                    submit(1, 1, Side::Buy, 100, 1)).result,
                    TradingResult::Accepted);
            }
            const TradingBootstrapConfig reordered{{
                BootstrapAccount{3, {}},
                BootstrapAccount{
                    2,
                    {{instrument.quote_asset, {initial_funds, 0}},
                     {instrument.base_asset, {initial_funds, 0}}}},
                BootstrapAccount{
                    1,
                    {{instrument.quote_asset, {initial_funds, 0}},
                     {instrument.base_asset, {initial_funds, 0}}}},
            }};

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(instrument, path, reordered);

            EXPECT_TRUE(recovered->order_book().find_order(1).has_value());
        }

        TEST(ExecutionRecoveryTest,
             InitialReservedBalanceIsCompatibleAcrossRestart) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            const TradingBootstrapConfig bootstrap{{BootstrapAccount{
                1,
                {{instrument.quote_asset, {900, 100}}}}}};
            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument, path, bootstrap);
                EXPECT_EQ(
                    static_cast<const TradingRuntime&>(*runtime)
                        .accounts().find_balance(1, instrument.quote_asset),
                    (Balance{900, 100}));
            }

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(instrument, path, bootstrap);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(1, instrument.quote_asset),
                (Balance{900, 100}));
        }

        TEST(ExecutionRecoveryTest, RejectsChangedBootstrapState) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            { std::unique_ptr<TradingRuntime> runtime = open_runtime(path); }

            TradingBootstrapConfig changed_balance = bootstrap_config();
            ++changed_balance.accounts[0].balances[0].balance.available;
            expect_bootstrap_mismatch(path, changed_balance);

            TradingBootstrapConfig missing_account = bootstrap_config();
            missing_account.accounts.pop_back();
            expect_bootstrap_mismatch(path, missing_account);

            TradingBootstrapConfig extra_account = bootstrap_config();
            extra_account.accounts.push_back(BootstrapAccount{4, {}});
            expect_bootstrap_mismatch(path, extra_account);

            TradingBootstrapConfig changed_asset = bootstrap_config();
            changed_asset.accounts[0].balances[0].asset_id = 30;
            expect_bootstrap_mismatch(path, changed_asset);
        }

        TEST(ExecutionRecoveryTest, BootstrapMismatchPrecedesCommandReplay) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{2, Side::Buy, OrderType::Limit, 100, 1, 1}});
            }
            TradingBootstrapConfig changed = bootstrap_config();
            ++changed.accounts[0].balances[0].balance.available;

            expect_bootstrap_mismatch(path, changed);
        }

        TEST(ExecutionRecoveryTest,
             DurableRecordWithoutPriorApplyIsAppliedAtRestart) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{1, Side::Buy, OrderType::Limit, 100, 2, 1}});
            }

            std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
            EXPECT_TRUE(recovered->order_book().find_order(1).has_value());
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(1, 10),
                (Balance{9'800, 200}));
            EXPECT_EQ(recovered->ledger().entries().size(), 1U);
        }

        TEST(ExecutionRecoveryTest,
             RejectedDurableSubmitRestoresExecutionSequenceGap) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    99,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 1}});
            }

            std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
            const TradingResponse next = recovered->executor().execute(
                submit(2, 1, Side::Buy, 100, 1));
            EXPECT_EQ(next.assigned_order_id, 2U);
            const auto& accepted = std::get<OrderAccepted>(
                next.events.front().payload);
            EXPECT_EQ(accepted.order.timestamp, 2);
        }

        TEST(ExecutionRecoveryTest,
             TornTailIsTruncatedReplayedAndFollowedByCleanAppend) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime = open_runtime(path);
                ASSERT_EQ(runtime->executor().execute(
                    submit(1, 1, Side::Buy, 100, 1)).result,
                    TradingResult::Accepted);
            }
            const WalRecordEncodeResult encoded = encode_wal_record(
                WalRecord{
                    2,
                    CancelExecutionCommand{2, 1, 1}},
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            WalBytes partial = std::get<WalBytes>(encoded);
            partial.resize(partial.size() - 1);
            append_raw(path, partial);

            {
                std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
                EXPECT_TRUE(recovered->order_book().find_order(1).has_value());
                EXPECT_EQ(recovered->executor().execute(
                    cancel(2, 1, 1)).result,
                    TradingResult::Cancelled);
            }

            const WalScanResult scan = scan_execution_wal(
                read_file(path),
                instrument,
                bootstrap_fingerprint());
            EXPECT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 2U);
            EXPECT_EQ(scan.records.back().sequence, 2U);
        }

        TEST(ExecutionRecoveryTest, RejectsExecutionIdentityGap) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{2, Side::Buy, OrderType::Limit, 100, 1, 1}});
            }

            try {
                static_cast<void>(open_runtime(path));
                FAIL() << "execution identity gap was accepted";
            } catch (const ExecutionRecoveryException& error) {
                EXPECT_EQ(
                    error.failure(),
                    ExecutionRecoveryFailure::ExecutionIdentityMismatch);
                EXPECT_EQ(error.wal_sequence(), 1U);
            }
        }

        TEST(ExecutionRecoveryTest, RejectsLogicalTimestampMismatch) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 2}});
            }

            EXPECT_THROW(
                static_cast<void>(open_runtime(path)),
                ExecutionRecoveryException);
        }

        TEST(ExecutionRecoveryTest, RejectsReusedExecutionIdentity) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(SubmitExecutionCommand{
                    1,
                    99,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 1}});
                writer.append(SubmitExecutionCommand{
                    2,
                    99,
                    Order{1, Side::Buy, OrderType::Limit, 100, 1, 1}});
            }

            EXPECT_THROW(
                static_cast<void>(open_runtime(path)),
                ExecutionRecoveryException);
        }

        TEST(ExecutionRecoveryTest,
             UnexpectedApplyFailureFailsStartupWithRecordContext) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            const TradingBootstrapConfig overflowing_bootstrap{{
                BootstrapAccount{1, {{20, {1, 0}}}},
                BootstrapAccount{
                    2,
                    {{10, {100, 0}},
                     {20, {std::numeric_limits<Amount>::max(), 0}}}},
            }};
            {
                ExecutionWalWriter writer{
                    path,
                    instrument,
                    calculate_bootstrap_fingerprint(
                        overflowing_bootstrap)};
                writer.append(SubmitExecutionCommand{
                    1,
                    1,
                    Order{1, Side::Sell, OrderType::Limit, 100, 1, 1}});
                writer.append(SubmitExecutionCommand{
                    2,
                    2,
                    Order{2, Side::Buy, OrderType::Limit, 100, 1, 2}});
            }

            try {
                static_cast<void>(TradingRuntime::create_durable(
                    instrument,
                    path,
                    overflowing_bootstrap));
                FAIL() << "recovery application failure was accepted";
            } catch (const ExecutionRecoveryException& error) {
                EXPECT_EQ(
                    error.failure(),
                    ExecutionRecoveryFailure::CommandApplication);
                EXPECT_EQ(error.wal_sequence(), 2U);
            }
        }

        TEST(ExecutionRecoveryTest,
             PreviouslyPoisoningCancelCanRecoverFromCleanBootstrap) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()};
                writer.append(CancelExecutionCommand{1, 1, 44});
            }

            std::unique_ptr<TradingRuntime> recovered = open_runtime(path);
            const TradingResponse next = recovered->executor().execute(
                submit(2, 1, Side::Buy, 100, 1));
            EXPECT_EQ(next.assigned_order_id, 1U);
        }

        TEST(ExecutionRecoveryTest, InstrumentMismatchFailsBeforeReplay) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path,
                    InstrumentContext{20, 10, 2, 1, 1},
                    bootstrap_fingerprint()};
            }

            EXPECT_THROW(
                static_cast<void>(open_runtime(path)),
                ExecutionWalWriterException);
        }

        TEST(ExecutionRecoveryTest, StartupRejectsChecksumCorruption) {
            expect_runtime_scan_failure(
                [](WalBytes& record) { record[20] ^= 0x80U; },
                WalError::ChecksumMismatch);
        }

        TEST(ExecutionRecoveryTest, StartupRejectsWalSequenceGap) {
            TemporaryDirectory directory;
            const std::string path = directory.wal_path();
            { ExecutionWalWriter writer{path, instrument, bootstrap_fingerprint()}; }
            const WalRecordEncodeResult encoded = encode_wal_record(
                WalRecord{2, CancelExecutionCommand{1, 1, 1}},
                instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            append_raw(path, std::get<WalBytes>(encoded));

            try {
                static_cast<void>(open_runtime(path));
                FAIL() << "WAL sequence gap was accepted";
            } catch (const ExecutionWalWriterException& error) {
                EXPECT_EQ(error.wal_error(), WalError::SequenceMismatch);
            }
        }

        TEST(ExecutionRecoveryTest, StartupRejectsUnknownRecordType) {
            expect_runtime_scan_failure(
                [](WalBytes& record) {
                    record[6] = 99;
                    rewrite_checksum(record);
                },
                WalError::UnknownRecordType);
        }

        TEST(ExecutionRecoveryTest, StartupRejectsRecordVersion) {
            expect_runtime_scan_failure(
                [](WalBytes& record) {
                    record[4] = 2;
                    record[5] = 0;
                    rewrite_checksum(record);
                },
                WalError::UnsupportedRecordVersion);
        }

        TEST(ExecutionRecoveryTest, StartupRejectsInvalidDurableCommand) {
            expect_runtime_scan_failure(
                [](WalBytes& record) {
                    for (std::size_t index = 16; index < 24; ++index) {
                        record[index] = 0;
                    }
                    rewrite_checksum(record);
                },
                WalError::InvalidValue);
        }
    }  // namespace
}  // namespace exchange
