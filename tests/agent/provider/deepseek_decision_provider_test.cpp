#include "agent/provider/deepseek/deepseek_decision_provider.hpp"

#include "accounting/account_store.hpp"
#include "accounting/execution_coordinator.hpp"
#include "accounting/ledger.hpp"
#include "accounting/order_reservation_store.hpp"
#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/agent_execution_adapter.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "execution/execution_sequencer.hpp"
#include "execution/trading_request_executor.hpp"
#include "matching/event_collector.hpp"
#include "matching/matching_engine.hpp"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        AgentObservation sample_observation() {
            AgentObservation observation;
            observation.agent_id = 101;
            observation.account_id = 7;
            observation.world = WorldState{
                3,
                InternalMarketState{90, 95},
                ExternalMarketState{
                    ExternalMarketSource::BinanceAlpha,
                    "ALPHA_426USDT",
                    ExternalPrice{4'070'000, 8},
                    ExternalPrice{4'060'000, 8},
                    ExternalPrice{4'080'000, 8},
                    1'773'110'025'000,
                    1'773'110'025'010,
                    ExternalMarketFreshness::Fresh},
            };
            observation.base_balance = Balance{1, 2};
            observation.quote_balance = Balance{700, 50};
            observation.active_orders.push_back(
                ObservedOrder{12, Side::Sell, 110, 2});
            observation.objective =
                ObjectiveProgress{101, 20, 3, 5, false};
            observation.economic_profile.max_order_quantity = 4;
            observation.economic_profile.max_order_notional = 500;
            observation.economic_profile.max_base_position = 8;
            observation.economic_profile.max_buy_price = 100;
            observation.economic_profile.min_sell_price = 80;
            observation.preference_profile =
                AgentPreferenceProfile{5, 120, 10};
            observation.contracts.push_back(Contract{
                7,
                202,
                101,
                ContractTerms{
                    101,
                    202,
                    500,
                    ResourceKind::ComputeCredit,
                    10},
                PaymentObligation{101, 202, 500, false},
                ResourceDeliveryObligation{
                    202,
                    101,
                    ResourceKind::ComputeCredit,
                    10,
                    false},
                ContractState::Proposed});
            return observation;
        }

        TEST(DeepSeekDecisionProviderTest,
             UsesProviderInterfaceAndParsesStructuredAction) {
            DeepSeekPrompt captured;
            DeepSeekDecisionProvider deepseek(
                [&](const DeepSeekPrompt& prompt) {
                    captured = prompt;
                    return std::string(
                        R"({"action":"submit_order","side":"buy","price":95,"quantity":2})");
                });
            const AgentDecisionProvider& provider = deepseek;

            EXPECT_EQ(
                provider.decide(sample_observation()),
                (AgentAction{SubmitOrderAction{Side::Buy, 95, 2}}));
            EXPECT_NE(captured.system.find("one JSON object"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("cannot modify balances"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("Step: 3"), std::string::npos);
            EXPECT_NE(captured.user.find("Agent ID: 101"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("id=12"), std::string::npos);
            EXPECT_NE(captured.user.find("target=5"), std::string::npos);
            EXPECT_NE(captured.system.find("not trading on Binance"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("internal exchange"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("deterministic runtime"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("symbol=ALPHA_426USDT"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("latest_trade=0.04070000"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("max_order_quantity=4"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("max_buy_price=100"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("quote_unit_value=1"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("target_base_inventory=5"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("base_unit_value=120"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("propose_contract"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("accept_contract"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("reject_contract"),
                      std::string::npos);
            EXPECT_NE(
                captured.system.find("fulfill_resource_obligation"),
                std::string::npos);
            EXPECT_NE(
                captured.system.find("\"action\":\"settle"),
                std::string::npos);
            EXPECT_NE(captured.system.find("Only an Accepted contract"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("resource-delivery debtor"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("synthetic internal resource"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("Only a Fulfilled contract"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("payment debtor"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("reserved quote is not spendable"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("Insufficient available quote"),
                      std::string::npos);
            EXPECT_NE(captured.system.find("blockchain, wallet, or x402"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("Relevant contracts:"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("id=7"), std::string::npos);
            EXPECT_NE(captured.user.find("state=proposed"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("payment_amount=500"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("resource=compute_credit"),
                      std::string::npos);
            EXPECT_NE(captured.user.find("resource_debtor=202"),
                      std::string::npos);
            EXPECT_NE(
                captured.user.find(
                    "inventory_deviation_penalty_per_unit=10"),
                std::string::npos);
        }

        TEST(DeepSeekDecisionProviderTest,
             DescribesMissingExternalMarketAsUnavailable) {
            AgentObservation observation = sample_observation();
            observation.world.external_market.reset();

            const DeepSeekPrompt prompt = build_deepseek_prompt(observation);

            EXPECT_NE(
                prompt.user.find("External market: unavailable\n"),
                std::string::npos);
            EXPECT_EQ(
                prompt.user.find("source=Binance Alpha"),
                std::string::npos);
        }

        class CountingExecutionAdapter final : public AgentExecutionAdapter {
        public:
            AgentActionResult execute(
                AgentId,
                const AgentAction&) override {
                ++calls;
                return HoldActionResult{};
            }

            std::size_t calls{};
        };

        TEST(DeepSeekDecisionProviderTest,
             StructurallyValidEconomicViolationIsRejectedByRuntime) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            AgentRegistry registry;
            ContractStore contracts;
            ASSERT_TRUE(accounts.create_account(7));
            ASSERT_TRUE(registry.register_agent({101, 7}));
            const AgentObservationService observations{
                registry,
                accounts,
                reservations,
                matching_engine.order_book(),
                contracts,
                instrument};
            CountingExecutionAdapter execution;
            DeepSeekDecisionProvider deepseek([](const DeepSeekPrompt&) {
                return std::string(
                    R"({"action":"submit_order","side":"buy","price":101,"quantity":1})");
            });
            AgentEconomicProfile profile;
            profile.max_buy_price = 100;
            AgentRuntime runtime(
                {{101, &deepseek, std::nullopt, profile}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(1);

            ASSERT_EQ(runtime.trace().size(), 1U);
            EXPECT_EQ(
                runtime.trace().front().validation,
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                runtime.trace().front().economic_constraint,
                AgentEconomicConstraintResult::BuyPriceExceeded);
            EXPECT_EQ(
                runtime.trace().front().status,
                AgentTurnStatus::EconomicConstraintRejected);
            EXPECT_EQ(execution.calls, 0U);
        }

        TEST(DeepSeekDecisionProviderTest,
             BadButAllowedActionExecutesAndRecordsNegativeUtility) {
            constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
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
            TradingRequestExecutor request_executor{
                instrument,
                coordinator,
                events,
                sequencer};
            ASSERT_TRUE(accounts.create_account(7));
            ASSERT_TRUE(accounts.create_account(8));
            accounts.fund(7, instrument.quote_asset, 1'000);
            accounts.fund(8, instrument.base_asset, 1);
            const TradingResponse liquidity = request_executor.execute(
                TradingRequest{
                    1,
                    8,
                    SubmitTradingRequest{Side::Sell, 100, 1}});
            ASSERT_EQ(liquidity.result, TradingResult::Accepted);

            AgentRegistry registry;
            ASSERT_TRUE(registry.register_agent({101, 7}));
            ContractStore contracts;
            ContractSequencer contract_sequencer;
            ContractCommandApplier contract_applier{
                contracts,
                accounts,
                ledger,
                instrument.quote_asset};
            ContractRequestExecutor contract_executor{
                registry,
                contracts,
                contract_sequencer,
                contract_applier};
            const AgentObservationService observations{
                registry,
                accounts,
                reservations,
                matching_engine.order_book(),
                contracts,
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                registry,
                request_executor,
                contract_executor,
                2};
            DeepSeekDecisionProvider deepseek([](const DeepSeekPrompt&) {
                return std::string(
                    R"({"action":"submit_order","side":"buy","price":100,"quantity":1})");
            });
            AgentRuntime runtime(
                {{101,
                  &deepseek,
                  std::nullopt,
                  {},
                  AgentPreferenceProfile{0, 50, 0}}},
                observations,
                execution,
                instrument);

            runtime.run_step_at(1);

            ASSERT_EQ(runtime.trace().size(), 1U);
            const AgentTurnRecord& turn = runtime.trace().front();
            EXPECT_EQ(turn.status, AgentTurnStatus::Executed);
            EXPECT_EQ(turn.validation, AgentActionValidationResult::Valid);
            EXPECT_EQ(
                turn.economic_constraint,
                AgentEconomicConstraintResult::Allowed);
            ASSERT_TRUE(turn.utility_before.has_value());
            ASSERT_TRUE(turn.utility_after.has_value());
            EXPECT_EQ(turn.utility_before->total, 1'000);
            EXPECT_EQ(turn.utility_after->total, 950);
            EXPECT_EQ(turn.utility_delta, -50);
            EXPECT_EQ(
                accounts.find_balance(7, instrument.base_asset),
                (Balance{1, 0}));
            EXPECT_EQ(
                accounts.find_balance(7, instrument.quote_asset),
                (Balance{900, 0}));
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(101);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->negative_utility_turns, 1U);
            EXPECT_EQ(metrics->cumulative_utility_delta, -50);
        }

        TEST(DeepSeekActionParsingTest, ParsesHoldAndCancel) {
            EXPECT_EQ(
                parse_deepseek_action(R"({"action":"hold"})"),
                AgentAction{HoldAction{}});
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"cancel_order","order_id":12})"),
                AgentAction{CancelOrderAction{12}});
        }

        TEST(DeepSeekActionParsingTest, ParsesContractActions) {
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":500,"resource":"compute_credit","resource_quantity":10})"),
                (AgentAction{ProposeContractAction{
                    202,
                    ContractTerms{
                        101,
                        202,
                        500,
                        ResourceKind::ComputeCredit,
                        10}}}));
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"accept_contract","contract_id":7})"),
                AgentAction{AcceptContractAction{7}});
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"reject_contract","contract_id":7})"),
                AgentAction{RejectContractAction{7}});
            EXPECT_EQ(
                parse_deepseek_action(
                    R"({"action":"fulfill_resource_obligation","contract_id":7})"),
                AgentAction{FulfillResourceObligationAction{7}});
        }

        TEST(DeepSeekActionParsingTest, RejectsMalformedJsonAndUnknownAction) {
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action("not json")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"wait"})")),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekActionParsingTest, RejectsMissingAndUnexpectedFields) {
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"submit_order","side":"buy","price":95})")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"hold","quantity":1})")),
                DeepSeekDecisionError);
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(
                    R"({"action":"submit_order","side":"buy","price":95,"quantity":1,"account_id":9})")),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekActionParsingTest, RejectsInvalidQuantityAndPrice) {
            for (const std::string& response : {
                     R"({"action":"submit_order","side":"buy","price":0,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":0})",
                     R"({"action":"submit_order","side":"buy","price":-1,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":-1})",
                     R"({"action":"submit_order","side":"BUY","price":1,"quantity":1})"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_deepseek_action(response)),
                    DeepSeekDecisionError);
            }

            const std::string out_of_range =
                R"({"action":"submit_order","side":"buy","price":)"
                + std::to_string(
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    + 1)
                + R"(,"quantity":1})";
            EXPECT_THROW(
                static_cast<void>(parse_deepseek_action(out_of_range)),
                DeepSeekDecisionError);
        }

        TEST(DeepSeekActionParsingTest,
             RejectsMalformedContractActions) {
            for (const std::string& response : {
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":500,"resource":"compute_credit"})",
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":500,"resource":"compute_credit","resource_quantity":10,"message":"accept this"})",
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":500,"resource":"gpu","resource_quantity":10})",
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":0,"resource":"compute_credit","resource_quantity":10})",
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":500,"resource":"compute_credit","resource_quantity":0})",
                     R"({"action":"propose_contract","counterparty":"202","payer":101,"payee":202,"payment_amount":500,"resource":"compute_credit","resource_quantity":10})",
                     R"({"action":"propose_contract","counterparty":202,"payer":101,"payee":202,"payment_amount":9223372036854775808,"resource":"compute_credit","resource_quantity":10})",
                     R"({"action":"accept_contract","contract_id":0})",
                     R"({"action":"accept_contract","contract_id":"7"})",
                     R"({"action":"accept_contract","contract_id":18446744073709551615})",
                     R"({"action":"reject_contract","contract_id":-1})",
                     R"({"action":"fulfill_resource_obligation"})",
                     R"({"action":"fulfill_resource_obligation","contract_id":0})",
                     R"({"action":"fulfill_resource_obligation","contract_id":"7"})",
                     R"({"action":"fulfill_resource_obligation","contract_id":7,"actor":202})",
                     R"({"action":"settle_payment_obligation"})",
                     R"({"action":"settle_payment_obligation","contract_id":0})",
                     R"({"action":"settle_payment_obligation","contract_id":"7"})",
                     R"({"action":"settle_payment_obligation","contract_id":7,"actor":101})",
                     R"({"action":"settle_payment_obligation","contract_id":7,"payment_amount":500})",
                     R"({"action":"accept_contract","contract_id":7,"agent_id":101})"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_deepseek_action(response)),
                    DeepSeekDecisionError);
            }
        }

        TEST(DeepSeekActionParsingTest,
             ParsesSettlementWithoutModelSuppliedActorOrAmount) {
            const AgentAction action = parse_deepseek_action(
                R"({"action":"settle_payment_obligation","contract_id":7})");
            ASSERT_TRUE(std::holds_alternative<
                        SettlePaymentObligationAction>(action));
            EXPECT_EQ(
                std::get<SettlePaymentObligationAction>(action)
                    .contract_id,
                7U);
        }

        TEST(DeepSeekDecisionProviderTest, RejectsMissingCompletionFunction) {
            EXPECT_THROW(
                static_cast<void>(DeepSeekDecisionProvider({})),
                std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
