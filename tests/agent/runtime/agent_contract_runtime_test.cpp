#include "../../support/wal_files.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AgentId agent_a = 101;
        constexpr AgentId agent_b = 202;
        constexpr AgentId agent_c = 303;
        constexpr AccountId account_a = 1;
        constexpr AccountId account_b = 2;
        constexpr AccountId account_c = 3;

        class MutableCapturingProvider final
            : public AgentDecisionProvider {
        public:
            explicit MutableCapturingProvider(AgentAction initial_action)
                : action(std::move(initial_action)) {}

            AgentAction decide(
                const AgentObservation& observation) const override {
                observations.push_back(observation);
                return action;
            }

            AgentAction action;
            mutable std::vector<AgentObservation> observations;
        };

        TradingBootstrapConfig bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    account_a,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {1'000, 0}}}},
                BootstrapAccount{
                    account_b,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {1'000, 0}}}},
                BootstrapAccount{
                    account_c,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {1'000, 0}}}},
            }};
        }

        void register_agents(TradingRuntime& runtime, bool include_c = false) {
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {agent_a, account_a}));
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {agent_b, account_b}));
            if (include_c) {
                ASSERT_TRUE(runtime.agent_registry().register_agent(
                    {agent_c, account_c}));
            }
        }

        ProposeContractAction proposal(AgentId counterparty = agent_b) {
            return ProposeContractAction{
                counterparty,
                ContractTerms{
                    agent_a,
                    counterparty,
                    500,
                    ResourceKind::ComputeCredit,
                    10}};
        }

        WalScanResult scan(const std::string& path) {
            return scan_execution_wal(
                test::read_file_bytes(path),
                instrument,
                calculate_bootstrap_fingerprint(bootstrap()));
        }

        AgentTurnRecord run_one_action(
            AgentId agent_id,
            AgentAction action,
            const AgentObservationService& observations,
            AgentExecutionAdapter& execution,
            std::int64_t timestamp) {
            MutableCapturingProvider provider{std::move(action)};
            AgentRuntime runtime(
                {{agent_id, &provider}},
                observations,
                execution,
                instrument);
            runtime.run_step_at(timestamp);
            EXPECT_EQ(runtime.trace().size(), 1U);
            return runtime.trace().front();
        }

        TEST(AgentContractRuntimeTest,
             ProposalAndAcceptanceAreDurableVisibleAndIsolated) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*exchange);
            const TradingRuntime& view = *exchange;
            const AccountStore::AccountBalances balances_before =
                view.accounts().entries();
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};
            MutableCapturingProvider proposer{proposal()};
            MutableCapturingProvider counterparty{AcceptContractAction{1}};
            const AgentPreferenceProfile preference{10, 100, 5};
            AgentRuntime runtime(
                {{agent_a,
                  &proposer,
                  std::nullopt,
                  {},
                  preference},
                 {agent_b,
                  &counterparty,
                  std::nullopt,
                  {},
                  preference}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(10);

            ASSERT_EQ(runtime.trace().size(), 2U);
            const AgentTurnRecord& proposed = runtime.trace()[0];
            ASSERT_TRUE(proposed.execution_result.has_value());
            EXPECT_EQ(
                std::get<ContractActionResult>(*proposed.execution_result),
                (ContractActionResult{1, ContractResult::Success}));
            EXPECT_EQ(proposed.status, AgentTurnStatus::Executed);
            EXPECT_EQ(
                proposed.economic_constraint,
                AgentEconomicConstraintResult::Allowed);
            EXPECT_EQ(proposed.utility_delta, 0);
            ASSERT_EQ(counterparty.observations.size(), 1U);
            ASSERT_EQ(counterparty.observations.front().contracts.size(), 1U);
            EXPECT_EQ(
                counterparty.observations.front().contracts.front().state,
                ContractState::Proposed);

            const AgentTurnRecord& accepted = runtime.trace()[1];
            ASSERT_TRUE(accepted.execution_result.has_value());
            EXPECT_EQ(
                std::get<ContractActionResult>(*accepted.execution_result),
                (ContractActionResult{1, ContractResult::Success}));
            EXPECT_EQ(accepted.status, AgentTurnStatus::Executed);
            EXPECT_EQ(accepted.utility_delta, 0);
            ASSERT_TRUE(exchange->contracts().find(1).has_value());
            EXPECT_EQ(
                exchange->contracts().find(1)->state,
                ContractState::Accepted);

            EXPECT_EQ(view.accounts().entries(), balances_before);
            EXPECT_TRUE(exchange->reservations().entries().empty());
            EXPECT_EQ(exchange->order_book().order_count(), 0U);
            EXPECT_TRUE(exchange->ledger().entries().empty());

            const PerAgentExperimentMetrics* proposer_metrics =
                runtime.metrics().find_agent(agent_a);
            const PerAgentExperimentMetrics* counterparty_metrics =
                runtime.metrics().find_agent(agent_b);
            ASSERT_NE(proposer_metrics, nullptr);
            ASSERT_NE(counterparty_metrics, nullptr);
            EXPECT_EQ(proposer_metrics->contract_proposals, 1U);
            EXPECT_EQ(proposer_metrics->successful_contract_actions, 1U);
            EXPECT_EQ(counterparty_metrics->contract_accepts, 1U);
            EXPECT_EQ(counterparty_metrics->successful_contract_actions, 1U);
            const SocietyExperimentMetrics society = runtime.metrics().society();
            EXPECT_EQ(society.total_contract_proposals, 1U);
            EXPECT_EQ(society.total_contract_accepts, 1U);
            EXPECT_EQ(society.total_successful_contract_actions, 2U);

            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            ASSERT_EQ(wal.records.size(), 2U);
            EXPECT_TRUE(std::holds_alternative<
                        CreateContractExecutionCommand>(
                wal.records[0].command));
            EXPECT_TRUE(std::holds_alternative<
                        AcceptContractExecutionCommand>(
                wal.records[1].command));

            proposer.action = HoldAction{};
            counterparty.action = HoldAction{};
            runtime.run_step_at(11);
            ASSERT_EQ(proposer.observations.size(), 2U);
            ASSERT_EQ(proposer.observations.back().contracts.size(), 1U);
            EXPECT_EQ(
                proposer.observations.back().contracts.front().state,
                ContractState::Accepted);
        }

        TEST(AgentContractRuntimeTest,
             RejectionIsVisibleToProposerOnNextStep) {
            test::TempTestDirectory directory;
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    directory.wal_path(),
                    bootstrap());
            register_agents(*exchange);
            const TradingRuntime& view = *exchange;
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};
            MutableCapturingProvider proposer{proposal()};
            MutableCapturingProvider counterparty{RejectContractAction{1}};
            AgentRuntime runtime(
                {{agent_a, &proposer}, {agent_b, &counterparty}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(20);
            ASSERT_EQ(
                exchange->contracts().find(1)->state,
                ContractState::Rejected);
            proposer.action = HoldAction{};
            counterparty.action = HoldAction{};
            runtime.run_step_at(21);

            ASSERT_EQ(proposer.observations.back().contracts.size(), 1U);
            EXPECT_EQ(
                proposer.observations.back().contracts.front().state,
                ContractState::Rejected);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_b);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->contract_rejects, 1U);
            EXPECT_EQ(metrics->successful_contract_actions, 1U);
        }

        TEST(AgentContractRuntimeTest,
             ResourceDebtorFulfillsDurablyAndLaterAgentSeesItSameStep) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*exchange);
            const TradingRuntime& view = *exchange;
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};

            ASSERT_EQ(
                run_one_action(
                    agent_a,
                    proposal(),
                    observations,
                    execution,
                    22).status,
                AgentTurnStatus::Executed);
            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    AcceptContractAction{1},
                    observations,
                    execution,
                    23).status,
                AgentTurnStatus::Executed);

            const AccountStore::AccountBalances balances_before =
                view.accounts().entries();
            MutableCapturingProvider resource_debtor{
                FulfillResourceObligationAction{1}};
            MutableCapturingProvider other_party{HoldAction{}};
            const AgentPreferenceProfile preference{10, 100, 5};
            AgentRuntime runtime(
                {{agent_b,
                  &resource_debtor,
                  std::nullopt,
                  {},
                  preference},
                 {agent_a,
                  &other_party,
                  std::nullopt,
                  {},
                  preference}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(24);

            ASSERT_EQ(runtime.trace().size(), 2U);
            const AgentTurnRecord& fulfilled_turn = runtime.trace().front();
            ASSERT_TRUE(fulfilled_turn.execution_result.has_value());
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *fulfilled_turn.execution_result),
                (ContractActionResult{1, ContractResult::Success}));
            EXPECT_EQ(fulfilled_turn.status, AgentTurnStatus::Executed);
            EXPECT_EQ(fulfilled_turn.utility_delta, 0);

            ASSERT_EQ(resource_debtor.observations.size(), 1U);
            ASSERT_EQ(resource_debtor.observations.front().contracts.size(),
                      1U);
            const Contract& accepted =
                resource_debtor.observations.front().contracts.front();
            EXPECT_EQ(accepted.state, ContractState::Accepted);
            EXPECT_FALSE(accepted.resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(accepted.payment_obligation.fulfilled);

            ASSERT_EQ(other_party.observations.size(), 1U);
            ASSERT_EQ(other_party.observations.front().contracts.size(), 1U);
            const Contract& visible =
                other_party.observations.front().contracts.front();
            EXPECT_EQ(visible.state, ContractState::Fulfilled);
            EXPECT_TRUE(visible.resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(visible.payment_obligation.fulfilled);
            EXPECT_EQ(visible.resource_delivery_obligation.debtor, agent_b);
            EXPECT_EQ(visible.resource_delivery_obligation.creditor, agent_a);
            EXPECT_EQ(
                visible.resource_delivery_obligation.resource,
                ResourceKind::ComputeCredit);
            EXPECT_EQ(visible.resource_delivery_obligation.quantity, 10);

            const std::optional<Contract> authoritative =
                exchange->contracts().find(1);
            ASSERT_TRUE(authoritative.has_value());
            EXPECT_EQ(authoritative->state, ContractState::Fulfilled);
            EXPECT_TRUE(
                authoritative->resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(authoritative->payment_obligation.fulfilled);
            EXPECT_TRUE(exchange->contracts().invariants_hold());

            EXPECT_EQ(view.accounts().entries(), balances_before);
            EXPECT_TRUE(exchange->reservations().entries().empty());
            EXPECT_EQ(exchange->order_book().order_count(), 0U);
            EXPECT_TRUE(exchange->ledger().entries().empty());

            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_b);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->contract_fulfillment_attempts, 1U);
            EXPECT_EQ(metrics->successful_contract_fulfillments, 1U);
            EXPECT_EQ(metrics->contract_fulfillment_rejections, 0U);
            const SocietyExperimentMetrics society = runtime.metrics().society();
            EXPECT_EQ(society.total_contract_fulfillment_attempts, 1U);
            EXPECT_EQ(society.total_successful_contract_fulfillments, 1U);
            EXPECT_EQ(society.total_contract_fulfillment_rejections, 0U);

            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            ASSERT_EQ(wal.records.size(), 3U);
            EXPECT_TRUE(std::holds_alternative<
                        FulfillResourceObligationExecutionCommand>(
                wal.records.back().command));
        }

        TEST(AgentContractRuntimeTest,
             PaymentDebtorSettlesAndUtilityReflectsExactQuoteTransfer) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*exchange);
            const TradingRuntime& view = *exchange;
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};
            ASSERT_EQ(
                run_one_action(
                    agent_a,
                    proposal(),
                    observations,
                    execution,
                    25).status,
                AgentTurnStatus::Executed);
            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    AcceptContractAction{1},
                    observations,
                    execution,
                    26).status,
                AgentTurnStatus::Executed);
            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    FulfillResourceObligationAction{1},
                    observations,
                    execution,
                    27).status,
                AgentTurnStatus::Executed);

            const AgentTurnRecord wrong_actor = run_one_action(
                agent_b,
                SettlePaymentObligationAction{1},
                observations,
                execution,
                28);
            EXPECT_EQ(wrong_actor.status,
                      AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *wrong_actor.execution_result),
                (ContractActionResult{
                    1,
                    ContractResult::UnauthorizedActor}));
            EXPECT_EQ(scan(path).records.size(), 3U);

            MutableCapturingProvider payer{
                SettlePaymentObligationAction{1}};
            MutableCapturingProvider payee{HoldAction{}};
            const AgentPreferenceProfile preference{10, 100, 5};
            AgentRuntime runtime(
                {{agent_a,
                  &payer,
                  std::nullopt,
                  {},
                  preference},
                 {agent_b,
                  &payee,
                  std::nullopt,
                  {},
                  preference}},
                observations,
                execution,
                instrument);
            runtime.run_step_at(29);

            ASSERT_EQ(runtime.trace().size(), 2U);
            const AgentTurnRecord& settled_turn = runtime.trace().front();
            EXPECT_EQ(settled_turn.status, AgentTurnStatus::Executed);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *settled_turn.execution_result),
                (ContractActionResult{1, ContractResult::Success}));
            EXPECT_EQ(settled_turn.utility_delta, -500);
            EXPECT_EQ(
                settled_turn.post_state.quote_balance,
                (Balance{500, 0}));

            ASSERT_EQ(payee.observations.size(), 1U);
            ASSERT_EQ(payee.observations.front().contracts.size(), 1U);
            EXPECT_EQ(
                payee.observations.front().contracts.front().state,
                ContractState::Settled);
            EXPECT_EQ(
                payee.observations.front().quote_balance,
                (Balance{1'500, 0}));

            const Contract contract = *exchange->contracts().find(1);
            EXPECT_EQ(contract.state, ContractState::Settled);
            EXPECT_TRUE(contract.resource_delivery_obligation.fulfilled);
            EXPECT_TRUE(contract.payment_obligation.fulfilled);
            EXPECT_EQ(
                view.accounts().find_balance(
                    account_a, instrument.quote_asset),
                (Balance{500, 0}));
            EXPECT_EQ(
                view.accounts().find_balance(
                    account_b, instrument.quote_asset),
                (Balance{1'500, 0}));
            EXPECT_TRUE(exchange->reservations().entries().empty());
            EXPECT_EQ(exchange->order_book().order_count(), 0U);
            ASSERT_EQ(exchange->ledger().entries().size(), 1U);
            EXPECT_TRUE(std::holds_alternative<
                        ContractSettlementLedgerMetadata>(
                exchange->ledger().entries().front()
                    .transaction.metadata));

            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->contract_settlement_attempts, 1U);
            EXPECT_EQ(metrics->successful_contract_settlements, 1U);
            EXPECT_EQ(metrics->contract_settlement_rejections, 0U);
            const SocietyExperimentMetrics society =
                runtime.metrics().society();
            EXPECT_EQ(society.total_contract_settlement_attempts, 1U);
            EXPECT_EQ(society.total_successful_contract_settlements, 1U);
            EXPECT_EQ(society.total_contract_settlement_rejections, 0U);

            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            ASSERT_EQ(wal.records.size(), 4U);
            ASSERT_TRUE(std::holds_alternative<
                        SettlePaymentObligationExecutionCommand>(
                wal.records.back().command));
            const auto command = std::get<
                SettlePaymentObligationExecutionCommand>(
                wal.records.back().command);
            EXPECT_EQ(command.contract_id, 1U);
            EXPECT_EQ(command.acting_agent, agent_a);
            EXPECT_EQ(command.payer_account_id, account_a);
            EXPECT_EQ(command.payee_account_id, account_b);
        }

        TEST(AgentContractRuntimeTest,
             DomainFailuresProduceNormalTurnsAndNoWalRecords) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*exchange, true);
            const TradingRuntime& view = *exchange;
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};

            const AgentTurnRecord created = run_one_action(
                agent_a,
                proposal(),
                observations,
                execution,
                30);
            EXPECT_EQ(created.status, AgentTurnStatus::Executed);

            const AgentTurnRecord wrong_accept = run_one_action(
                agent_c,
                AcceptContractAction{1},
                observations,
                execution,
                31);
            EXPECT_TRUE(wrong_accept.observation.contracts.empty());
            EXPECT_EQ(wrong_accept.status, AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *wrong_accept.execution_result).status,
                ContractResult::UnauthorizedActor);

            const AgentTurnRecord wrong_reject = run_one_action(
                agent_c,
                RejectContractAction{1},
                observations,
                execution,
                32);
            EXPECT_EQ(wrong_reject.status, AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *wrong_reject.execution_result).status,
                ContractResult::UnauthorizedActor);

            const AgentTurnRecord accepted = run_one_action(
                agent_b,
                AcceptContractAction{1},
                observations,
                execution,
                33);
            EXPECT_EQ(accepted.status, AgentTurnStatus::Executed);

            const std::vector<std::pair<AgentAction, ContractResult>>
                invalid_actions{
                    {AcceptContractAction{1},
                     ContractResult::InvalidTransition},
                    {RejectContractAction{1},
                     ContractResult::InvalidTransition},
                    {AcceptContractAction{999},
                     ContractResult::ContractNotFound}};
            for (const auto& [invalid, expected] : invalid_actions) {
                const AgentTurnRecord turn = run_one_action(
                    agent_b,
                    invalid,
                    observations,
                    execution,
                    34);
                EXPECT_EQ(turn.status, AgentTurnStatus::ExecutionRejected);
                ASSERT_TRUE(turn.execution_result.has_value());
                EXPECT_EQ(
                    std::get<ContractActionResult>(
                        *turn.execution_result).status,
                    expected);
            }

            const AgentTurnRecord unregistered = run_one_action(
                agent_a,
                proposal(999),
                observations,
                execution,
                35);
            EXPECT_EQ(unregistered.status, AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *unregistered.execution_result).status,
                ContractResult::InvalidTerms);

            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            EXPECT_EQ(wal.records.size(), 2U);
        }

        TEST(AgentContractRuntimeTest,
             FulfillmentDomainFailuresAreNormalAndNotJournaled) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*exchange, true);
            const TradingRuntime& view = *exchange;
            AgentObservationService observations{
                exchange->agent_registry(),
                view.accounts(),
                exchange->reservations(),
                exchange->order_book(),
                exchange->contracts(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange->agent_registry(),
                exchange->executor(),
                exchange->contract_executor()};

            ASSERT_EQ(
                run_one_action(
                    agent_a,
                    proposal(),
                    observations,
                    execution,
                    36).status,
                AgentTurnStatus::Executed);
            const AgentTurnRecord proposed = run_one_action(
                agent_b,
                FulfillResourceObligationAction{1},
                observations,
                execution,
                37);
            EXPECT_EQ(proposed.status, AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *proposed.execution_result).status,
                ContractResult::InvalidTransition);

            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    RejectContractAction{1},
                    observations,
                    execution,
                    38).status,
                AgentTurnStatus::Executed);
            const AgentTurnRecord rejected = run_one_action(
                agent_b,
                FulfillResourceObligationAction{1},
                observations,
                execution,
                39);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *rejected.execution_result).status,
                ContractResult::InvalidTransition);

            ASSERT_EQ(
                run_one_action(
                    agent_a,
                    proposal(),
                    observations,
                    execution,
                    40).status,
                AgentTurnStatus::Executed);
            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    AcceptContractAction{2},
                    observations,
                    execution,
                    41).status,
                AgentTurnStatus::Executed);

            const std::vector<std::pair<AgentId, ContractId>> invalid{
                {agent_a, 2},
                {agent_c, 2},
                {agent_b, 999}};
            const std::vector<ContractResult> expected{
                ContractResult::UnauthorizedActor,
                ContractResult::UnauthorizedActor,
                ContractResult::ContractNotFound};
            for (std::size_t index = 0; index < invalid.size(); ++index) {
                const AgentTurnRecord turn = run_one_action(
                    invalid[index].first,
                    FulfillResourceObligationAction{invalid[index].second},
                    observations,
                    execution,
                    42 + static_cast<std::int64_t>(index));
                EXPECT_EQ(turn.status, AgentTurnStatus::ExecutionRejected);
                EXPECT_EQ(
                    std::get<ContractActionResult>(
                        *turn.execution_result).status,
                    expected[index]);
            }

            ASSERT_EQ(
                run_one_action(
                    agent_b,
                    FulfillResourceObligationAction{2},
                    observations,
                    execution,
                    45).status,
                AgentTurnStatus::Executed);
            const AgentTurnRecord repeated = run_one_action(
                agent_b,
                FulfillResourceObligationAction{2},
                observations,
                execution,
                46);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *repeated.execution_result).status,
                ContractResult::InvalidTransition);

            ASSERT_EQ(
                exchange->contract_executor().settle_payment(2, agent_a),
                ContractResult::Success);
            const AgentTurnRecord settled = run_one_action(
                agent_b,
                FulfillResourceObligationAction{2},
                observations,
                execution,
                47);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *settled.execution_result).status,
                ContractResult::InvalidTransition);

            const WalScanResult wal = scan(path);
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            EXPECT_EQ(wal.records.size(), 6U);
        }

        TEST(AgentContractRuntimeTest,
             RecoveredProposalIsObservedAndAcceptedAfterReregistration) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> exchange =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                register_agents(*exchange);
                const TradingRuntime& view = *exchange;
                AgentObservationService observations{
                    exchange->agent_registry(),
                    view.accounts(),
                    exchange->reservations(),
                    exchange->order_book(),
                    exchange->contracts(),
                    instrument};
                TradingRequestAgentExecutionAdapter execution{
                    exchange->agent_registry(),
                    exchange->executor(),
                    exchange->contract_executor()};
                const AgentTurnRecord proposed = run_one_action(
                    agent_a,
                    proposal(),
                    observations,
                    execution,
                    40);
                EXPECT_EQ(proposed.status, AgentTurnStatus::Executed);
            }

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*recovered);
            const TradingRuntime& view = *recovered;
            AgentObservationService observations{
                recovered->agent_registry(),
                view.accounts(),
                recovered->reservations(),
                recovered->order_book(),
                recovered->contracts(),
                instrument};
            const AgentObservation observed = observations.observe(
                agent_b,
                observations.capture_world(1));
            ASSERT_EQ(observed.contracts.size(), 1U);
            EXPECT_EQ(observed.contracts.front().state,
                      ContractState::Proposed);

            TradingRequestAgentExecutionAdapter execution{
                recovered->agent_registry(),
                recovered->executor(),
                recovered->contract_executor()};
            const AgentTurnRecord accepted = run_one_action(
                agent_b,
                AcceptContractAction{1},
                observations,
                execution,
                41);
            EXPECT_EQ(accepted.status, AgentTurnStatus::Executed);
            ASSERT_TRUE(recovered->contracts().find(1).has_value());
            EXPECT_EQ(recovered->contracts().find(1)->state,
                      ContractState::Accepted);
        }

        TEST(AgentContractRuntimeTest,
             RecoveredFulfillmentIsVisibleAfterReregistration) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            {
                std::unique_ptr<TradingRuntime> exchange =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap());
                register_agents(*exchange);
                const TradingRuntime& view = *exchange;
                AgentObservationService observations{
                    exchange->agent_registry(),
                    view.accounts(),
                    exchange->reservations(),
                    exchange->order_book(),
                    exchange->contracts(),
                    instrument};
                TradingRequestAgentExecutionAdapter execution{
                    exchange->agent_registry(),
                    exchange->executor(),
                    exchange->contract_executor()};
                ASSERT_EQ(
                    run_one_action(
                        agent_a,
                        proposal(),
                        observations,
                        execution,
                        50).status,
                    AgentTurnStatus::Executed);
                ASSERT_EQ(
                    run_one_action(
                        agent_b,
                        AcceptContractAction{1},
                        observations,
                        execution,
                        51).status,
                    AgentTurnStatus::Executed);
                ASSERT_EQ(
                    run_one_action(
                        agent_b,
                        FulfillResourceObligationAction{1},
                        observations,
                        execution,
                        52).status,
                    AgentTurnStatus::Executed);
            }

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap());
            register_agents(*recovered);
            const TradingRuntime& view = *recovered;
            AgentObservationService observations{
                recovered->agent_registry(),
                view.accounts(),
                recovered->reservations(),
                recovered->order_book(),
                recovered->contracts(),
                instrument};

            for (const AgentId agent : {agent_a, agent_b}) {
                const AgentObservation observed = observations.observe(
                    agent,
                    observations.capture_world(1));
                ASSERT_EQ(observed.contracts.size(), 1U);
                const Contract& contract = observed.contracts.front();
                EXPECT_EQ(contract.state, ContractState::Fulfilled);
                EXPECT_TRUE(
                    contract.resource_delivery_obligation.fulfilled);
                EXPECT_FALSE(contract.payment_obligation.fulfilled);
            }
            EXPECT_TRUE(recovered->contracts().invariants_hold());
        }
    }  // namespace
}  // namespace exchange
