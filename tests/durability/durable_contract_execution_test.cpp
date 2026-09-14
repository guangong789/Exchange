#include "../support/wal_files.hpp"
#include "execution/trading_runtime.hpp"

#include "agent/domain/utility.hpp"
#include "durability/command_journal.hpp"
#include "durability/execution_recovery.hpp"
#include "durability/execution_wal.hpp"
#include "durability/execution_wal_writer.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AgentId proposer = 101;
        constexpr AgentId counterparty = 202;

        TradingBootstrapConfig bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {10'000, 0}}}},
                BootstrapAccount{
                    2,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {10'000, 0}}}},
            }};
        }

        void register_agents(TradingRuntime& runtime) {
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {proposer, 1}));
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {counterparty, 2}));
        }

        CreateContractRequest create_request(Amount payment = 500) {
            return CreateContractRequest{
                proposer,
                counterparty,
                ContractTerms{
                    proposer,
                    counterparty,
                    payment,
                    ResourceKind::ComputeCredit,
                    20}};
        }

        WalScanResult scan(const std::string& path) {
            return scan_execution_wal(
                test::read_file_bytes(path),
                instrument,
                calculate_bootstrap_fingerprint(bootstrap()));
        }

        ContractId create(TradingRuntime& runtime) {
            const ContractExecutionResponse result =
                runtime.contract_executor().create_contract(
                    create_request());
            EXPECT_EQ(result.result, ContractResult::Success);
            EXPECT_TRUE(result.contract_id.has_value());
            return result.contract_id.value_or(0);
        }

        void expect_same_order(
            const std::optional<Order>& actual,
            const std::optional<Order>& expected) {
            ASSERT_EQ(actual.has_value(), expected.has_value());
            if (!actual.has_value()) {
                return;
            }
            EXPECT_EQ(actual->id, expected->id);
            EXPECT_EQ(actual->side, expected->side);
            EXPECT_EQ(actual->type, expected->type);
            EXPECT_EQ(actual->price, expected->price);
            EXPECT_EQ(actual->quantity, expected->quantity);
            EXPECT_EQ(actual->timestamp, expected->timestamp);
        }

        TEST(DurableContractRuntimeTest,
             RecoversEveryLifecycleStateAndNextContractId) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                register_agents(*runtime);
                EXPECT_EQ(create(*runtime), 1U);

                const ContractId accepted = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().accept_contract(
                        accepted,
                        counterparty),
                    ContractResult::Success);

                const ContractId rejected = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().reject_contract(
                        rejected,
                        counterparty),
                    ContractResult::Success);

                const ContractId fulfilled = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().accept_contract(
                        fulfilled,
                        counterparty),
                    ContractResult::Success);
                ASSERT_EQ(
                    runtime->contract_executor().fulfill_resource(
                        fulfilled,
                        counterparty),
                    ContractResult::Success);

                const ContractId settled = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().accept_contract(
                        settled,
                        counterparty),
                    ContractResult::Success);
                ASSERT_EQ(
                    runtime->contract_executor().fulfill_resource(
                        settled,
                        counterparty),
                    ContractResult::Success);
                ASSERT_EQ(
                    runtime->contract_executor().settle_payment(
                        settled,
                        proposer),
                    ContractResult::Success);
            }

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            ASSERT_EQ(recovered->contracts().size(), 5U);
            EXPECT_EQ(
                recovered->contracts().find(1)->state,
                ContractState::Proposed);
            EXPECT_EQ(
                recovered->contracts().find(2)->state,
                ContractState::Accepted);
            EXPECT_EQ(
                recovered->contracts().find(3)->state,
                ContractState::Rejected);
            EXPECT_EQ(
                recovered->contracts().find(4)->state,
                ContractState::Fulfilled);
            const Contract settled = *recovered->contracts().find(5);
            EXPECT_EQ(settled.state, ContractState::Settled);
            EXPECT_TRUE(settled.resource_delivery_obligation.fulfilled);
            EXPECT_TRUE(settled.payment_obligation.fulfilled);
            EXPECT_EQ(settled.terms, create_request().terms);
            EXPECT_TRUE(recovered->contracts().invariants_hold());
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    1, instrument.quote_asset),
                (Balance{9'500, 0}));
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().find_balance(
                    2, instrument.quote_asset),
                (Balance{10'500, 0}));
            ASSERT_EQ(recovered->ledger().entries().size(), 1U);
            EXPECT_EQ(recovered->ledger().entries()[0].sequence, 1U);
            EXPECT_TRUE(std::holds_alternative<
                        ContractSettlementLedgerMetadata>(
                recovered->ledger().entries()[0].transaction.metadata));

            register_agents(*recovered);
            const auto balances_after_recovery =
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().entries();
            EXPECT_EQ(
                recovered->contract_executor().settle_payment(
                    5,
                    proposer),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().entries(),
                balances_after_recovery);
            EXPECT_EQ(recovered->ledger().entries().size(), 1U);
            EXPECT_EQ(scan(path).records.size(), 12U);

            EXPECT_EQ(create(*recovered), 6U);
            ASSERT_EQ(
                recovered->contract_executor().accept_contract(
                    6,
                    counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                recovered->contract_executor().fulfill_resource(
                    6,
                    counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                recovered->contract_executor().settle_payment(
                    6,
                    proposer),
                ContractResult::Success);
            ASSERT_EQ(recovered->ledger().entries().size(), 2U);
            EXPECT_EQ(recovered->ledger().entries()[1].sequence, 2U);
            EXPECT_EQ(scan(path).records.back().sequence, 16U);
        }

        TEST(DurableContractRuntimeTest,
             RejectionsArePreAdmissionAndConsumeNoDurableIdentity) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());

            EXPECT_EQ(
                runtime->contract_executor().create_contract(
                    create_request()).result,
                ContractResult::InvalidTerms);
            register_agents(*runtime);
            EXPECT_EQ(
                runtime->contract_executor().create_contract(
                    create_request(0)).result,
                ContractResult::InvalidTerms);
            EXPECT_EQ(scan(path).records.size(), 0U);

            const ContractId id = create(*runtime);
            EXPECT_EQ(id, 1U);
            EXPECT_EQ(
                runtime->contract_executor().accept_contract(id, proposer),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id,
                    counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                runtime->contract_executor().accept_contract(
                    999,
                    counterparty),
                ContractResult::ContractNotFound);
            EXPECT_EQ(scan(path).records.size(), 1U);

            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id,
                    counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                runtime->contract_executor().accept_contract(
                    id,
                    counterparty),
                ContractResult::InvalidTransition);
            EXPECT_EQ(scan(path).records.size(), 2U);
            EXPECT_EQ(create(*runtime), 2U);

            const TradingResponse order = runtime->executor().execute(
                TradingRequest{
                    1,
                    1,
                    SubmitTradingRequest{Side::Buy, 90, 1}});
            ASSERT_EQ(order.result, TradingResult::Accepted);
            EXPECT_EQ(order.assigned_order_id, 1U);
            EXPECT_EQ(
                std::get<OrderAccepted>(order.events.front().payload)
                    .order.timestamp,
                1);
        }

        TEST(DurableContractRuntimeTest,
             MixedTradingAndContractHistoryRecoversInOneWalOrder) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            AccountStore::AccountBalances expected_accounts;
            std::map<OrderId, OrderReservation> expected_reservations;
            std::vector<LedgerEntry> expected_ledger;
            std::optional<Order> expected_first_order;
            std::optional<Order> expected_second_order;
            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                register_agents(*runtime);
                ASSERT_EQ(
                    runtime->executor().execute(TradingRequest{
                        1,
                        1,
                        SubmitTradingRequest{Side::Buy, 90, 1}}).result,
                    TradingResult::Accepted);
                EXPECT_EQ(create(*runtime), 1U);
                ASSERT_EQ(
                    runtime->executor().execute(TradingRequest{
                        2,
                        2,
                        SubmitTradingRequest{Side::Sell, 110, 1}}).result,
                    TradingResult::Accepted);
                ASSERT_EQ(
                    runtime->contract_executor().accept_contract(
                        1,
                        counterparty),
                    ContractResult::Success);
                const TradingRuntime& view = *runtime;
                expected_accounts = view.accounts().entries();
                expected_reservations = view.reservations().entries();
                expected_ledger = view.ledger().entries();
                expected_first_order = view.order_book().find_order(1);
                expected_second_order = view.order_book().find_order(2);
            }

            const WalScanResult records = scan(path);
            ASSERT_EQ(records.status, WalScanStatus::CleanEof);
            ASSERT_EQ(records.records.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<SubmitExecutionCommand>(
                records.records[0].command));
            EXPECT_TRUE(std::holds_alternative<CreateContractExecutionCommand>(
                records.records[1].command));
            EXPECT_TRUE(std::holds_alternative<SubmitExecutionCommand>(
                records.records[2].command));
            EXPECT_TRUE(std::holds_alternative<AcceptContractExecutionCommand>(
                records.records[3].command));

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            ASSERT_EQ(recovered->contracts().find(1)->state,
                      ContractState::Accepted);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().entries(),
                expected_accounts);
            EXPECT_EQ(
                recovered->reservations().entries(),
                expected_reservations);
            EXPECT_EQ(recovered->ledger().entries(), expected_ledger);
            expect_same_order(
                recovered->order_book().find_order(1),
                expected_first_order);
            expect_same_order(
                recovered->order_book().find_order(2),
                expected_second_order);

            register_agents(*recovered);
            EXPECT_EQ(create(*recovered), 2U);
            const TradingResponse next = recovered->executor().execute(
                TradingRequest{
                    3,
                    1,
                    SubmitTradingRequest{Side::Buy, 80, 1}});
            EXPECT_EQ(next.assigned_order_id, 3U);
            EXPECT_EQ(
                std::get<OrderAccepted>(next.events.front().payload)
                    .order.timestamp,
                3);
        }

        TEST(DurableContractRuntimeTest,
             SameBootstrapAndWalRecoverIdenticalContractState) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> runtime =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                register_agents(*runtime);
                const ContractId fulfilled = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().accept_contract(
                        fulfilled,
                        counterparty),
                    ContractResult::Success);
                ASSERT_EQ(
                    runtime->contract_executor().fulfill_resource(
                        fulfilled,
                        counterparty),
                    ContractResult::Success);
                const ContractId rejected = create(*runtime);
                ASSERT_EQ(
                    runtime->contract_executor().reject_contract(
                        rejected,
                        counterparty),
                    ContractResult::Success);
            }

            std::vector<Contract> first_recovery;
            {
                std::unique_ptr<TradingRuntime> recovered =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                first_recovery.push_back(*recovered->contracts().find(1));
                first_recovery.push_back(*recovered->contracts().find(2));
            }
            std::unique_ptr<TradingRuntime> recovered_again =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            ASSERT_EQ(recovered_again->contracts().size(), 2U);
            EXPECT_EQ(
                *recovered_again->contracts().find(1),
                first_recovery[0]);
            EXPECT_EQ(
                *recovered_again->contracts().find(2),
                first_recovery[1]);
            EXPECT_TRUE(recovered_again->contracts().invariants_hold());

            register_agents(*recovered_again);
            EXPECT_EQ(create(*recovered_again), 3U);
        }

        TEST(DurableContractRuntimeTest,
             SettlementTransfersExactQuoteAndLeavesMatchingStateIsolated) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*runtime);
            const AccountStore::AccountBalances balances_before =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries();
            AgentObservation observation_before;
            observation_before.base_balance =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().find_balance(1, instrument.base_asset);
            observation_before.quote_balance =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().find_balance(1, instrument.quote_asset);
            const AgentPreferenceProfile preference{10, 100, 5};
            const AgentUtilityBreakdown utility_before =
                evaluate_agent_utility(observation_before, preference);

            const ContractId id = create(*runtime);
            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id,
                    counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id,
                    counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries(),
                balances_before);
            EXPECT_TRUE(runtime->ledger().entries().empty());
            ASSERT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::Success);

            const TradingRuntime& view = *runtime;
            EXPECT_EQ(
                view.accounts().find_balance(1, instrument.quote_asset),
                (Balance{9'500, 0}));
            EXPECT_EQ(
                view.accounts().find_balance(2, instrument.quote_asset),
                (Balance{10'500, 0}));
            EXPECT_EQ(
                view.accounts().find_balance(1, instrument.base_asset),
                (Balance{10, 0}));
            EXPECT_EQ(
                view.accounts().find_balance(2, instrument.base_asset),
                (Balance{10, 0}));
            EXPECT_EQ(runtime->order_book().order_count(), 0U);
            EXPECT_TRUE(runtime->reservations().entries().empty());
            ASSERT_EQ(runtime->ledger().entries().size(), 1U);
            const LedgerEntry& settlement =
                runtime->ledger().entries().front();
            EXPECT_EQ(settlement.sequence, 1U);
            ASSERT_TRUE(std::holds_alternative<
                        ContractSettlementLedgerMetadata>(
                settlement.transaction.metadata));
            EXPECT_EQ(
                std::get<ContractSettlementLedgerMetadata>(
                    settlement.transaction.metadata),
                (ContractSettlementLedgerMetadata{id, 1, 2}));
            EXPECT_EQ(
                settlement.transaction.postings,
                (std::vector<Posting>{
                    {1, instrument.quote_asset,
                     BalanceBucket::Available, -500},
                    {2, instrument.quote_asset,
                     BalanceBucket::Available, 500}}));
            EXPECT_FALSE(std::holds_alternative<TradeLedgerMetadata>(
                settlement.transaction.metadata));
            AgentObservation observation_after;
            observation_after.base_balance = view.accounts().find_balance(
                1,
                instrument.base_asset);
            observation_after.quote_balance = view.accounts().find_balance(
                1,
                instrument.quote_asset);
            const AgentUtilityBreakdown utility_after =
                evaluate_agent_utility(observation_after, preference);
            EXPECT_EQ(
                utility_after.total - utility_before.total,
                -500);
            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            EXPECT_EQ(wal.records.size(), 4U);
        }

        TEST(DurableContractRuntimeTest,
             SettlementSpendsAvailableQuoteWithoutReleasingReservation) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*runtime);
            ASSERT_EQ(
                runtime->executor().execute(TradingRequest{
                    1,
                    1,
                    SubmitTradingRequest{Side::Buy, 9'300, 1}}).result,
                TradingResult::Accepted);
            const ContractId id = create(*runtime);
            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id, counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id, counterparty),
                ContractResult::Success);

            ASSERT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::Success);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().find_balance(
                    1, instrument.quote_asset),
                (Balance{200, 9'300}));
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().find_balance(
                    2, instrument.quote_asset),
                (Balance{10'500, 0}));
            EXPECT_EQ(runtime->reservations().entries().size(), 1U);
            EXPECT_EQ(runtime->order_book().order_count(), 1U);
            ASSERT_EQ(runtime->ledger().entries().size(), 2U);
            EXPECT_TRUE(std::holds_alternative<ReserveLedgerMetadata>(
                runtime->ledger().entries()[0].transaction.metadata));
            EXPECT_TRUE(std::holds_alternative<
                        ContractSettlementLedgerMetadata>(
                runtime->ledger().entries()[1].transaction.metadata));
            EXPECT_EQ(runtime->ledger().entries()[1].sequence, 2U);
        }

        TEST(DurableContractRuntimeTest,
             ReservedQuoteCannotCoverSettlementAndRejectionIsPreWal) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*runtime);
            ASSERT_EQ(
                runtime->executor().execute(TradingRequest{
                    1,
                    1,
                    SubmitTradingRequest{Side::Buy, 9'600, 1}}).result,
                TradingResult::Accepted);
            const ContractId id = create(*runtime);
            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id, counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id, counterparty),
                ContractResult::Success);
            const auto accounts_before =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries();
            const auto reservations_before =
                runtime->reservations().entries();
            const auto ledger_before = runtime->ledger().entries();
            const auto wal_before = test::read_file_bytes(path);

            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::InsufficientFunds);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries(),
                accounts_before);
            EXPECT_EQ(runtime->reservations().entries(), reservations_before);
            EXPECT_EQ(runtime->ledger().entries(), ledger_before);
            EXPECT_EQ(test::read_file_bytes(path), wal_before);
            EXPECT_EQ(runtime->order_book().order_count(), 1U);
            const Contract contract = *runtime->contracts().find(id);
            EXPECT_EQ(contract.state, ContractState::Fulfilled);
            EXPECT_TRUE(contract.resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(contract.payment_obligation.fulfilled);
        }

        TEST(DurableContractRuntimeTest,
             SettlementBusinessPreflightRejectsWithoutMutationOrWal) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig overflow_bootstrap{{
                BootstrapAccount{
                    1,
                    {{instrument.quote_asset, {500, 0}}}},
                BootstrapAccount{
                    2,
                    {{instrument.quote_asset,
                      {std::numeric_limits<Amount>::max(), 0}}}},
            }};
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    overflow_bootstrap);
            register_agents(*runtime);
            const ContractId id = create(*runtime);

            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::InvalidTransition);
            EXPECT_EQ(
                runtime->contract_executor().settle_payment(999, proposer),
                ContractResult::ContractNotFound);
            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id, counterparty),
                ContractResult::Success);
            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::InvalidTransition);
            ASSERT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id, counterparty),
                ContractResult::Success);
            const auto accounts_before =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries();
            const auto wal_before = test::read_file_bytes(path);

            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, counterparty),
                ContractResult::UnauthorizedActor);
            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::BalanceOverflow);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries(),
                accounts_before);
            EXPECT_TRUE(runtime->ledger().entries().empty());
            EXPECT_EQ(test::read_file_bytes(path), wal_before);
            EXPECT_EQ(runtime->contracts().find(id)->state,
                      ContractState::Fulfilled);
        }

        TEST(DurableContractRuntimeTest,
             MissingSettlementAccountIsNormalPreWalRejection) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig one_account{{
                BootstrapAccount{
                    1,
                    {{instrument.quote_asset, {1'000, 0}}}},
            }};
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    one_account);
            register_agents(*runtime);
            const ContractId id = create(*runtime);
            ASSERT_EQ(
                runtime->contract_executor().accept_contract(
                    id, counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                runtime->contract_executor().fulfill_resource(
                    id, counterparty),
                ContractResult::Success);
            const auto accounts_before =
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries();
            const auto wal_before = test::read_file_bytes(path);

            EXPECT_EQ(
                runtime->contract_executor().settle_payment(id, proposer),
                ContractResult::AccountNotFound);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*runtime)
                    .accounts().entries(),
                accounts_before);
            EXPECT_TRUE(runtime->ledger().entries().empty());
            EXPECT_EQ(test::read_file_bytes(path), wal_before);
            EXPECT_EQ(runtime->contracts().find(id)->state,
                      ContractState::Fulfilled);
        }

        class ThrowingJournal final : public ExecutionCommandJournal {
        public:
            void append(const ExecutionCommand&) override {
                ++attempts;
                throw std::runtime_error("injected append failure");
            }

            std::size_t attempts{};
        };

        TEST(DurableContractRuntimeTest,
             JournalFailurePreventsApplyAndPoisonsSharedRuntimeStatus) {
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({proposer, 1}));
            ASSERT_TRUE(registry.register_agent({counterparty, 2}));
            ContractStore store;
            ContractSequencer sequencer;
            AccountStore accounts;
            Ledger ledger;
            ContractCommandApplier applier{
                store,
                accounts,
                ledger,
                instrument.quote_asset};
            ThrowingJournal journal;
            ExecutionRuntimeStatus status;
            ContractRequestExecutor executor{
                registry,
                store,
                sequencer,
                applier,
                &journal,
                &status};

            EXPECT_THROW(
                static_cast<void>(executor.create_contract(
                    create_request())),
                std::runtime_error);
            EXPECT_EQ(journal.attempts, 1U);
            EXPECT_EQ(store.size(), 0U);
            EXPECT_TRUE(executor.poisoned());
            EXPECT_TRUE(status.poisoned);
            EXPECT_TRUE(status.durable_processing_started);
            EXPECT_THROW(
                static_cast<void>(executor.create_contract(
                    create_request())),
                std::logic_error);
        }

        class ApplyInterferenceJournal final : public ExecutionCommandJournal {
        public:
            explicit ApplyInterferenceJournal(ContractStore& store)
                : store_(store) {}

            void append(const ExecutionCommand& command) override {
                const auto* create =
                    std::get_if<CreateContractExecutionCommand>(&command);
                if (create == nullptr
                    || store_.create_contract(
                           create->contract_id,
                           create->proposer,
                           create->counterparty,
                           create->terms)
                        != ContractResult::Success) {
                    throw std::logic_error(
                        "failed to inject contract apply interference");
                }
            }

        private:
            ContractStore& store_;
        };

        class SettlementInterferenceJournal final
            : public ExecutionCommandJournal {
        public:
            SettlementInterferenceJournal(
                AccountStore& accounts,
                AssetId quote_asset_id)
                : accounts_(accounts),
                  quote_asset_id_(quote_asset_id) {}

            void append(const ExecutionCommand& command) override {
                const auto* settlement = std::get_if<
                    SettlePaymentObligationExecutionCommand>(&command);
                if (settlement == nullptr
                    || accounts_.reserve(
                           settlement->payer_account_id,
                           quote_asset_id_,
                           500)
                        != ReserveResult::Success) {
                    throw std::logic_error(
                        "failed to inject settlement apply interference");
                }
            }

        private:
            AccountStore& accounts_;
            AssetId quote_asset_id_;
        };

        TEST(DurableContractRuntimeTest,
             PostWalSettlementMismatchPoisonsInsteadOfBusinessRejecting) {
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({proposer, 1}));
            ASSERT_TRUE(registry.register_agent({counterparty, 2}));
            ContractStore store;
            ASSERT_EQ(
                store.create_contract(
                    1,
                    proposer,
                    counterparty,
                    create_request().terms),
                ContractResult::Success);
            ASSERT_EQ(
                store.accept_contract(1, counterparty),
                ContractResult::Success);
            ASSERT_EQ(
                store.mark_fulfilled(1, counterparty),
                ContractResult::Success);
            AccountStore accounts;
            ASSERT_TRUE(accounts.create_account(1));
            ASSERT_TRUE(accounts.create_account(2));
            accounts.fund(1, instrument.quote_asset, 500);
            Ledger ledger;
            ContractCommandApplier applier{
                store,
                accounts,
                ledger,
                instrument.quote_asset};
            SettlementInterferenceJournal journal{
                accounts,
                instrument.quote_asset};
            ExecutionRuntimeStatus status;
            ContractSequencer sequencer;
            ContractRequestExecutor executor{
                registry,
                store,
                sequencer,
                applier,
                &journal,
                &status};

            EXPECT_THROW(
                static_cast<void>(
                    executor.settle_payment(1, proposer)),
                std::logic_error);
            EXPECT_TRUE(executor.poisoned());
            EXPECT_TRUE(status.poisoned);
            EXPECT_TRUE(status.durable_processing_started);
            EXPECT_EQ(store.find(1)->state, ContractState::Fulfilled);
            EXPECT_FALSE(store.find(1)->payment_obligation.fulfilled);
            EXPECT_TRUE(ledger.entries().empty());
            EXPECT_EQ(
                accounts.find_balance(1, instrument.quote_asset),
                (Balance{0, 500}));
            EXPECT_EQ(
                accounts.find_balance(2, instrument.quote_asset),
                std::nullopt);
            EXPECT_THROW(
                static_cast<void>(
                    executor.settle_payment(1, proposer)),
                std::logic_error);
        }

        TEST(DurableContractRuntimeTest,
             PostJournalApplyMismatchPoisonsAllSharedExecutors) {
            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({proposer, 1}));
            ASSERT_TRUE(registry.register_agent({counterparty, 2}));
            ContractStore store;
            ContractSequencer contract_sequencer;
            AccountStore accounts;
            Ledger ledger;
            ContractCommandApplier contract_applier{
                store,
                accounts,
                ledger,
                instrument.quote_asset};
            ApplyInterferenceJournal journal{store};
            ExecutionRuntimeStatus status;
            ContractRequestExecutor contracts{
                registry,
                store,
                contract_sequencer,
                contract_applier,
                &journal,
                &status};

            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            ExecutionCoordinator coordinator{
                instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            ExecutionSequencer execution_sequencer;
            TradingRequestExecutor trading{
                instrument,
                coordinator,
                events,
                execution_sequencer,
                nullptr,
                &status};

            EXPECT_THROW(
                static_cast<void>(contracts.create_contract(
                    create_request())),
                std::logic_error);
            EXPECT_TRUE(contracts.poisoned());
            EXPECT_TRUE(trading.poisoned());
            EXPECT_THROW(
                static_cast<void>(trading.execute(TradingRequest{
                    1,
                    1,
                    CancelTradingRequest{1}})),
                std::logic_error);
        }

        TEST(DurableContractRecoveryTest, RejectsContractIdentityGap) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path,
                    instrument,
                    calculate_bootstrap_fingerprint(bootstrap())};
                writer.append(CreateContractExecutionCommand{
                    2,
                    proposer,
                    counterparty,
                    create_request().terms});
            }

            try {
                static_cast<void>(TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap()));
                FAIL() << "ContractId gap was recovered";
            } catch (const ExecutionRecoveryException& error) {
                EXPECT_EQ(
                    error.failure(),
                    ExecutionRecoveryFailure::ContractIdentityMismatch);
                EXPECT_EQ(error.wal_sequence(), 1U);
            }
        }

        TEST(DurableContractRecoveryTest,
             RejectsDurableDomainRejectionDuringReplay) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                ExecutionWalWriter writer{
                    path,
                    instrument,
                    calculate_bootstrap_fingerprint(bootstrap())};
                writer.append(CreateContractExecutionCommand{
                    1,
                    proposer,
                    counterparty,
                    create_request().terms});
                writer.append(AcceptContractExecutionCommand{
                    1,
                    proposer});
            }

            try {
                static_cast<void>(TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap()));
                FAIL() << "durable unauthorized transition was recovered";
            } catch (const ExecutionRecoveryException& error) {
                EXPECT_EQ(
                    error.failure(),
                    ExecutionRecoveryFailure::CommandApplication);
                EXPECT_EQ(error.wal_sequence(), 2U);
            }
        }
    }  // namespace
}  // namespace exchange
