#include "../../support/wal_files.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"
#include "../../../apps/exchange_agent_society_smoke/final_observations.hpp"

#include <map>
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

        class ScriptedProvider final : public AgentDecisionProvider {
        public:
            explicit ScriptedProvider(std::vector<AgentAction> actions)
                : actions_(std::move(actions)) {}

            AgentAction decide(
                const AgentObservation& observation) const override {
                observations.push_back(observation);
                if (next_action_ == actions_.size()) {
                    return HoldAction{};
                }
                return actions_[next_action_++];
            }

            mutable std::vector<AgentObservation> observations;

        private:
            const std::vector<AgentAction> actions_;
            mutable std::size_t next_action_{};
        };

        TradingBootstrapConfig bootstrap(
            Amount payer_available = 1'000,
            Amount payer_reserved = 0) {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    account_a,
                    {{instrument.base_asset, {5, 0}},
                     {instrument.quote_asset,
                      {payer_available, payer_reserved}}}},
                BootstrapAccount{
                    account_b,
                    {{instrument.base_asset, {20, 0}},
                     {instrument.quote_asset, {100, 0}}}},
                BootstrapAccount{
                    account_c,
                    {{instrument.base_asset, {10, 0}},
                     {instrument.quote_asset, {700, 0}}}},
            }};
        }

        void register_agents(TradingRuntime& runtime) {
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {agent_a, account_a}));
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {agent_b, account_b}));
            ASSERT_TRUE(runtime.agent_registry().register_agent(
                {agent_c, account_c}));
        }

        AgentEconomicProfile profile(
            Quantity max_quantity,
            Amount max_notional,
            Amount max_position,
            Price max_buy_price,
            Price min_sell_price) {
            AgentEconomicProfile result;
            result.max_order_quantity = max_quantity;
            result.max_order_notional = max_notional;
            result.max_base_position = max_position;
            result.max_buy_price = max_buy_price;
            result.min_sell_price = min_sell_price;
            return result;
        }

        ProposeContractAction proposal() {
            return ProposeContractAction{
                agent_b,
                ContractTerms{
                    agent_a,
                    agent_b,
                    500,
                    ResourceKind::ComputeCredit,
                    10}};
        }

        std::map<ContractId, Contract> collect_contracts(
            const ContractStore& contracts) {
            std::map<ContractId, Contract> result;
            for (const AgentId agent_id : {agent_a, agent_b, agent_c}) {
                for (const Contract& contract :
                     contracts.find_relevant(agent_id)) {
                    result.emplace(contract.id, contract);
                }
            }
            return result;
        }

        TEST(AgentSocietyExperimentTest,
             FinalObservationIncludesLaterParticipantsTrade) {
            TradingRuntime exchange{instrument};
            ASSERT_TRUE(exchange.accounts().create_account(account_a));
            ASSERT_TRUE(exchange.accounts().create_account(account_b));
            exchange.accounts().fund(account_a, instrument.base_asset, 1);
            exchange.accounts().fund(account_b, instrument.quote_asset, 100);
            ASSERT_TRUE(exchange.agent_registry().register_agent({agent_a, account_a}));
            ASSERT_TRUE(exchange.agent_registry().register_agent({agent_b, account_b}));
            AgentObservationService observations{
                exchange.agent_registry(), exchange.accounts(),
                exchange.reservations(), exchange.order_book(),
                exchange.contracts(), instrument};
            TradingRequestAgentExecutionAdapter execution{
                exchange.agent_registry(), exchange.executor(),
                exchange.contract_executor()};
            ScriptedProvider seller{{SubmitOrderAction{Side::Sell, 100, 1}}};
            ScriptedProvider buyer{{SubmitOrderAction{Side::Buy, 100, 1}}};
            const std::vector<AgentRuntimeParticipant> participants{
                {agent_a, &seller, std::nullopt, {}, AgentPreferenceProfile{0, 50, 0}},
                {agent_b, &buyer, std::nullopt, {}, AgentPreferenceProfile{0, 50, 0}}};
            AgentRuntime runtime{participants, observations, execution, instrument};

            runtime.run_step_at(1);
            const auto trace_before = runtime.trace();
            const auto final = society_smoke::capture_final_observations(
                observations, participants, runtime.current_step());
            const auto& seller_final = final.at(agent_a);

            ASSERT_EQ(runtime.trace().size(), 2U);
            EXPECT_EQ(runtime.trace()[0].post_state.base_balance, (Balance{0, 1}));
            EXPECT_EQ(runtime.trace()[0].post_state.active_order_count, 1U);
            EXPECT_EQ(seller_final.base_balance, (Balance{0, 0}));
            EXPECT_EQ(seller_final.quote_balance, (Balance{100, 0}));
            EXPECT_TRUE(seller_final.active_orders.empty());
            EXPECT_EQ(seller_final.quote_balance,
                      exchange.accounts().find_balance(account_a, instrument.quote_asset));
            const auto final_utility = evaluate_agent_utility(
                seller_final, *seller_final.preference_profile).total;
            EXPECT_EQ(final_utility, 100);
            EXPECT_EQ(runtime.metrics().find_agent(agent_a)->cumulative_utility_delta, 0);
            EXPECT_EQ(calculate_agent_utility_delta(
                          runtime.trace()[0].utility_before->total, final_utility), 50);
            EXPECT_EQ(runtime.trace(), trace_before);
        }

        TEST(AgentSocietyExperimentTest,
             ScriptedThreeAgentLifecycleIsVisibleDurableAndRecoverable) {
            test::TempTestDirectory directory;
            const std::string path = directory.wal_path();
            const TradingBootstrapConfig bootstrap_config = bootstrap();
            AccountStore::AccountBalances final_accounts;
            std::map<ContractId, Contract> final_contracts;
            std::vector<LedgerEntry> final_ledger;
            {
                std::unique_ptr<TradingRuntime> exchange =
                    TradingRuntime::create_durable(
                        instrument,
                        path,
                        bootstrap_config);
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
                ScriptedProvider provider_a{{
                    proposal(),
                    HoldAction{},
                    SettlePaymentObligationAction{1}}};
                ScriptedProvider provider_b{{
                    AcceptContractAction{1},
                    FulfillResourceObligationAction{1},
                    HoldAction{}}};
                ScriptedProvider provider_c{{
                    HoldAction{},
                    HoldAction{},
                    HoldAction{}}};
                AgentRuntime runtime(
                    {{agent_a,
                      &provider_a,
                      AssetTargetObjective{instrument.base_asset, 12},
                      profile(3, 1'500, 15, 200, 50),
                      AgentPreferenceProfile{12, 130, 4}},
                     {agent_b,
                      &provider_b,
                      AssetTargetObjective{instrument.base_asset, 8},
                      profile(5, 2'000, 28, 160, 40),
                      AgentPreferenceProfile{8, 90, 2}},
                     {agent_c,
                      &provider_c,
                      AssetTargetObjective{instrument.base_asset, 10},
                      profile(2, 800, 14, 120, 70),
                      AgentPreferenceProfile{10, 110, 12}}},
                    observations,
                    execution,
                    instrument);

                runtime.run_step_at(1);
                runtime.run_step_at(2);
                runtime.run_step_at(3);

                ASSERT_EQ(runtime.trace().size(), 9U);
                for (const AgentTurnRecord& turn : runtime.trace()) {
                    EXPECT_FALSE(
                        turn.observation.world.external_market.has_value());
                    EXPECT_FALSE(turn.external_market_context.has_value());
                }
                ASSERT_EQ(provider_b.observations.size(), 3U);
                ASSERT_EQ(provider_b.observations[0].contracts.size(), 1U);
                EXPECT_EQ(provider_b.observations[0].contracts[0].state,
                          ContractState::Proposed);
                EXPECT_EQ(
                    runtime.trace()[0].contract_state_after_action,
                    ContractState::Proposed);
                EXPECT_EQ(
                    runtime.trace()[1].contract_state_after_action,
                    ContractState::Accepted);
                EXPECT_EQ(
                    runtime.trace()[4].contract_state_after_action,
                    ContractState::Fulfilled);
                EXPECT_EQ(
                    runtime.trace()[6].contract_state_after_action,
                    ContractState::Settled);
                EXPECT_EQ(runtime.trace()[6].utility_delta, -500);

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
                    (Balance{600, 0}));
                ASSERT_EQ(exchange->ledger().entries().size(), 1U);
                EXPECT_TRUE(std::holds_alternative<
                            ContractSettlementLedgerMetadata>(
                    exchange->ledger().entries().front().transaction.metadata));

                const AgentExperimentMetrics& metrics = runtime.metrics();
                const PerAgentExperimentMetrics* a = metrics.find_agent(agent_a);
                const PerAgentExperimentMetrics* b = metrics.find_agent(agent_b);
                ASSERT_NE(a, nullptr);
                ASSERT_NE(b, nullptr);
                EXPECT_EQ(a->contract_proposals, 1U);
                EXPECT_EQ(a->contract_settlement_attempts, 1U);
                EXPECT_EQ(a->successful_contract_settlements, 1U);
                EXPECT_EQ(b->contract_accepts, 1U);
                EXPECT_EQ(b->successful_contract_fulfillments, 1U);
                const SocietyExperimentMetrics society = metrics.society();
                EXPECT_EQ(society.total_turns, 9U);
                EXPECT_EQ(society.total_contract_proposals, 1U);
                EXPECT_EQ(society.total_contract_accepts, 1U);
                EXPECT_EQ(
                    society.total_successful_contract_fulfillments,
                    1U);
                EXPECT_EQ(
                    society.total_successful_contract_settlements,
                    1U);
                const auto& pairs = metrics.contract_pair_interactions();
                ASSERT_EQ(pairs.size(), 1U);
                EXPECT_EQ(
                    pairs.at({agent_a, agent_b}),
                    (ContractPairInteractionMetrics{1, 1}));

                final_accounts = view.accounts().entries();
                final_contracts = collect_contracts(exchange->contracts());
                final_ledger = exchange->ledger().entries();
            }

            const WalScanResult wal = scan_execution_wal(
                test::read_file_bytes(path),
                instrument,
                calculate_bootstrap_fingerprint(bootstrap_config));
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            ASSERT_EQ(wal.records.size(), 4U);
            EXPECT_TRUE(std::holds_alternative<
                        SettlePaymentObligationExecutionCommand>(
                wal.records.back().command));

            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    instrument,
                    path,
                    bootstrap_config);
            register_agents(*recovered);
            EXPECT_EQ(
                static_cast<const TradingRuntime&>(*recovered)
                    .accounts().entries(),
                final_accounts);
            EXPECT_EQ(collect_contracts(recovered->contracts()), final_contracts);
            EXPECT_EQ(recovered->ledger().entries(), final_ledger);
        }

        TEST(AgentSocietyExperimentTest,
             ScriptedInsufficientFundsLeavesContractFulfilled) {
            test::TempTestDirectory directory;
            const TradingBootstrapConfig bootstrap_config = bootstrap(400, 600);
            std::unique_ptr<TradingRuntime> exchange =
                TradingRuntime::create_durable(
                    instrument,
                    directory.wal_path(),
                    bootstrap_config);
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
            ScriptedProvider provider_a{{
                proposal(),
                HoldAction{},
                SettlePaymentObligationAction{1}}};
            ScriptedProvider provider_b{{
                AcceptContractAction{1},
                FulfillResourceObligationAction{1},
                HoldAction{}}};
            ScriptedProvider provider_c{{HoldAction{}, HoldAction{}, HoldAction{}}};
            AgentRuntime runtime(
                {{agent_a,
                  &provider_a,
                  std::nullopt,
                  profile(3, 1'500, 15, 200, 50),
                  AgentPreferenceProfile{12, 130, 4}},
                 {agent_b,
                  &provider_b,
                  std::nullopt,
                  profile(5, 2'000, 28, 160, 40),
                  AgentPreferenceProfile{8, 90, 2}},
                 {agent_c,
                  &provider_c,
                  std::nullopt,
                  profile(2, 800, 14, 120, 70),
                  AgentPreferenceProfile{10, 110, 12}}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(1);
            runtime.run_step_at(2);
            const auto balances_before_settlement = view.accounts().entries();
            runtime.run_step_at(3);

            const AgentTurnRecord& settlement_turn = runtime.trace()[6];
            EXPECT_EQ(settlement_turn.status,
                      AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                std::get<ContractActionResult>(
                    *settlement_turn.execution_result).status,
                ContractResult::InsufficientFunds);
            EXPECT_EQ(settlement_turn.contract_state_after_action,
                      ContractState::Fulfilled);
            const Contract contract = *exchange->contracts().find(1);
            EXPECT_EQ(contract.state, ContractState::Fulfilled);
            EXPECT_TRUE(contract.resource_delivery_obligation.fulfilled);
            EXPECT_FALSE(contract.payment_obligation.fulfilled);
            EXPECT_EQ(view.accounts().entries(), balances_before_settlement);
            EXPECT_EQ(
                view.accounts().find_balance(
                    account_a, instrument.quote_asset),
                (Balance{400, 600}));
            EXPECT_TRUE(exchange->ledger().entries().empty());
            const PerAgentExperimentMetrics* a =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(a, nullptr);
            EXPECT_EQ(a->contract_settlement_attempts, 1U);
            EXPECT_EQ(a->contract_settlement_rejections, 1U);
            EXPECT_EQ(
                runtime.metrics().society()
                    .total_contract_settlement_rejections,
                1U);

            const WalScanResult wal = scan_execution_wal(
                test::read_file_bytes(directory.wal_path()),
                instrument,
                calculate_bootstrap_fingerprint(bootstrap_config));
            ASSERT_EQ(wal.status, WalScanStatus::CleanEof);
            ASSERT_EQ(wal.records.size(), 3U);
            EXPECT_FALSE(std::holds_alternative<
                         SettlePaymentObligationExecutionCommand>(
                wal.records.back().command));
        }
    }  // namespace
}  // namespace exchange
