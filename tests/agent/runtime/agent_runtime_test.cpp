#include "agent/runtime/agent_runtime.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "accounting/execution_coordinator.hpp"
#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "matching/event_collector.hpp"
#include "matching/matching_engine.hpp"

namespace exchange {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AgentId agent_a = 101;
        constexpr AgentId agent_b = 202;
        constexpr AccountId account_a = 1;
        constexpr AccountId account_b = 2;

        class FixedDecisionProvider final : public AgentDecisionProvider {
        public:
            explicit FixedDecisionProvider(AgentAction action)
                : action_(std::move(action)) {}

            AgentAction decide(const AgentObservation&) const override {
                return action_;
            }

        private:
            AgentAction action_;
        };

        class FailingDecisionProvider final : public AgentDecisionProvider {
        public:
            AgentAction decide(const AgentObservation&) const override {
                throw AgentDecisionError("expected provider failure");
            }
        };

        class UnexpectedFailureProvider final
            : public AgentDecisionProvider {
        public:
            AgentAction decide(const AgentObservation&) const override {
                throw std::logic_error("unexpected implementation failure");
            }
        };

        class CapturingDecisionProvider final
            : public AgentDecisionProvider {
        public:
            AgentAction decide(
                const AgentObservation& observation) const override {
                observations.push_back(observation);
                return HoldAction{};
            }

            mutable std::vector<AgentObservation> observations;
        };

        class FakeExternalMarketFeed final : public ExternalMarketFeed {
        public:
            ExternalMarketState latest(
                std::int64_t local_now_ms) const override {
                requested_at.push_back(local_now_ms);
                return state;
            }

            ExternalMarketState state;
            mutable std::vector<std::int64_t> requested_at;
        };

        struct RuntimeWorld {
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
            AgentRegistry registry;
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
            AgentObservationService observations{
                registry,
                accounts,
                reservations,
                matching_engine.order_book(),
                contracts,
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                registry,
                request_executor,
                contract_executor};
        };

        void create_agent(
            RuntimeWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent({agent_id, account_id}));
        }

        TEST(AgentRuntimeTest,
             ExecutesProvidersSeriallyAgainstFreshObservations) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            create_agent(world, agent_b, account_b);
            world.accounts.fund(account_a, 20, 2);
            world.accounts.fund(account_b, 10, 200);
            FixedDecisionProvider seller(
                SubmitOrderAction{Side::Sell, 100, 2});
            FixedDecisionProvider buyer(
                SubmitOrderAction{Side::Buy, 100, 2});
            AgentRuntime runtime(
                {{agent_a, &seller, std::nullopt},
                 {agent_b,
                  &buyer,
                  std::nullopt,
                  {},
                  AgentPreferenceProfile{2, 120, 0}}},
                world.observations,
                world.execution,
                instrument);

            runtime.run_step();

            ASSERT_EQ(runtime.trace().size(), 2U);
            EXPECT_EQ(runtime.current_step(), 1U);
            EXPECT_EQ(runtime.trace()[0].status, AgentTurnStatus::Executed);
            EXPECT_EQ(
                runtime.trace()[0].economic_constraint,
                AgentEconomicConstraintResult::Allowed);
            EXPECT_FALSE(runtime.trace()[0]
                             .observation.world.internal_market.best_ask
                             .has_value());
            EXPECT_EQ(
                runtime.trace()[1]
                    .observation.world.internal_market.best_ask,
                100);
            ASSERT_TRUE(runtime.trace()[1].execution_result.has_value());
            EXPECT_EQ(
                std::get<SubmitActionResult>(
                    *runtime.trace()[1].execution_result)
                    .status,
                AgentSubmitStatus::Accepted);
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            EXPECT_EQ(
                world.accounts.find_balance(account_b, 20),
                (Balance{2, 0}));
            EXPECT_EQ(runtime.trace()[0].pre_state.active_order_count, 0U);
            EXPECT_EQ(runtime.trace()[0].post_state.active_order_count, 1U);
            EXPECT_FALSE(
                runtime.trace()[0].external_market_context.has_value());
            EXPECT_EQ(
                runtime.trace()[1].post_state.base_balance,
                (Balance{2, 0}));
            ASSERT_TRUE(runtime.trace()[1].utility_before.has_value());
            ASSERT_TRUE(runtime.trace()[1].utility_after.has_value());
            EXPECT_EQ(runtime.trace()[1].utility_before->total, 200);
            EXPECT_EQ(runtime.trace()[1].utility_after->total, 240);
            EXPECT_EQ(runtime.trace()[1].utility_delta, 40);
            const PerAgentExperimentMetrics* seller_metrics =
                runtime.metrics().find_agent(agent_a);
            const PerAgentExperimentMetrics* buyer_metrics =
                runtime.metrics().find_agent(agent_b);
            ASSERT_NE(seller_metrics, nullptr);
            ASSERT_NE(buyer_metrics, nullptr);
            EXPECT_EQ(seller_metrics->proposed_sells, 1U);
            EXPECT_EQ(buyer_metrics->proposed_buys, 1U);
            EXPECT_EQ(seller_metrics->successful_executions, 1U);
            EXPECT_EQ(buyer_metrics->successful_executions, 1U);
            EXPECT_EQ(runtime.metrics().society().total_turns, 2U);
            EXPECT_EQ(
                runtime.metrics().society().total_successful_executions,
                2U);
        }

        TEST(AgentRuntimeTest, RejectedActionDoesNotMutateExchangeState) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            world.accounts.fund(account_a, 10, 500);
            const Balance before =
                *world.accounts.find_balance(account_a, 10);
            FixedDecisionProvider invalid(
                SubmitOrderAction{Side::Buy, 0, 1});
            AgentRuntime runtime(
                {{agent_a, &invalid, std::nullopt}},
                world.observations,
                world.execution,
                instrument);

            runtime.run_step();

            ASSERT_EQ(runtime.trace().size(), 1U);
            EXPECT_EQ(
                runtime.trace()[0].status,
                AgentTurnStatus::StructuralValidationRejected);
            EXPECT_EQ(
                runtime.trace()[0].validation,
                AgentActionValidationResult::InvalidPrice);
            EXPECT_FALSE(
                runtime.trace()[0].economic_constraint.has_value());
            EXPECT_FALSE(runtime.trace()[0].execution_result.has_value());
            EXPECT_EQ(world.accounts.find_balance(account_a, 10), before);
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->structural_rejections, 1U);
        }

        TEST(AgentRuntimeTest,
             EconomicConstraintRejectionDoesNotReachExecution) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            world.accounts.fund(account_a, 10, 1'000);
            FixedDecisionProvider oversized(
                SubmitOrderAction{Side::Buy, 100, 3});
            AgentEconomicProfile profile;
            profile.max_order_quantity = 2;
            AgentRuntime runtime(
                {{agent_a,
                  &oversized,
                  std::nullopt,
                  profile,
                  AgentPreferenceProfile{0, 100, 5}}},
                world.observations,
                world.execution,
                instrument);

            runtime.run_step();

            ASSERT_EQ(runtime.trace().size(), 1U);
            const AgentTurnRecord& turn = runtime.trace().front();
            EXPECT_EQ(
                turn.status,
                AgentTurnStatus::EconomicConstraintRejected);
            EXPECT_EQ(
                turn.validation,
                AgentActionValidationResult::Valid);
            EXPECT_EQ(
                turn.economic_constraint,
                AgentEconomicConstraintResult::OrderQuantityExceeded);
            EXPECT_FALSE(turn.execution_result.has_value());
            EXPECT_EQ(turn.observation.economic_profile, profile);
            EXPECT_EQ(turn.utility_before, turn.utility_after);
            EXPECT_EQ(turn.utility_delta, 0);
            EXPECT_EQ(
                world.accounts.find_balance(account_a, 10),
                (Balance{1'000, 0}));
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->economic_constraint_rejections, 1U);
        }

        TEST(AgentRuntimeTest,
             ConstraintAllowedBusinessRejectionRemainsDistinct) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            FixedDecisionProvider unfunded(
                SubmitOrderAction{Side::Buy, 100, 1});
            AgentRuntime runtime(
                {{agent_a, &unfunded, std::nullopt}},
                world.observations,
                world.execution,
                instrument);

            runtime.run_step();

            ASSERT_EQ(runtime.trace().size(), 1U);
            const AgentTurnRecord& turn = runtime.trace().front();
            EXPECT_EQ(turn.status, AgentTurnStatus::ExecutionRejected);
            EXPECT_EQ(
                turn.economic_constraint,
                AgentEconomicConstraintResult::Allowed);
            ASSERT_TRUE(turn.execution_result.has_value());
            EXPECT_EQ(
                std::get<SubmitActionResult>(*turn.execution_result).status,
                AgentSubmitStatus::InsufficientFunds);
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->execution_rejections, 1U);
            EXPECT_EQ(metrics->total_accepted_quantity, 0);
        }

        TEST(AgentRuntimeTest, ProviderFailureIsRecordedWithoutMutation) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            world.accounts.fund(account_a, 10, 500);
            const Balance before =
                *world.accounts.find_balance(account_a, 10);
            FailingDecisionProvider failing;
            AgentRuntime runtime(
                {{agent_a, &failing, std::nullopt}},
                world.observations,
                world.execution,
                instrument);

            EXPECT_NO_THROW(runtime.run_step());

            ASSERT_EQ(runtime.trace().size(), 1U);
            EXPECT_EQ(
                runtime.trace()[0].status,
                AgentTurnStatus::DecisionFailed);
            EXPECT_FALSE(runtime.trace()[0].action.has_value());
            EXPECT_FALSE(
                runtime.trace()[0].economic_constraint.has_value());
            EXPECT_FALSE(runtime.trace()[0].execution_result.has_value());
            EXPECT_EQ(world.accounts.find_balance(account_a, 10), before);
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->decision_failures, 1U);
        }

        TEST(AgentRuntimeTest, UnexpectedProviderExceptionsStillPropagate) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            UnexpectedFailureProvider failing;
            AgentRuntime runtime(
                {{agent_a, &failing, std::nullopt}},
                world.observations,
                world.execution,
                instrument);

            EXPECT_THROW(runtime.run_step(), std::logic_error);
            EXPECT_TRUE(runtime.trace().empty());
            EXPECT_TRUE(runtime.metrics().per_agent().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(AgentRuntimeTest,
             ReadsFreshExternalFeedAndTreatsStaleStateAsUnavailable) {
            RuntimeWorld world;
            create_agent(world, agent_a, account_a);
            CapturingDecisionProvider provider;
            FakeExternalMarketFeed feed;
            feed.state = ExternalMarketState{
                ExternalMarketSource::BinanceAlpha,
                "ALPHA_426USDT",
                ExternalPrice{4'070'000, 8},
                ExternalPrice{4'060'000, 8},
                ExternalPrice{4'080'000, 8},
                1'000,
                1'010,
                ExternalMarketFreshness::Fresh};
            AgentRuntime runtime(
                {{agent_a,
                  &provider,
                  std::nullopt,
                  {},
                  AgentPreferenceProfile{0, 1, 1}}},
                world.observations,
                world.execution,
                instrument,
                &feed);

            runtime.run_step_at(1'020);
            feed.state.freshness = ExternalMarketFreshness::Stale;
            runtime.run_step_at(7'000);
            feed.state.freshness = ExternalMarketFreshness::Unavailable;
            runtime.run_step_at(8'000);

            ASSERT_EQ(provider.observations.size(), 3U);
            ASSERT_TRUE(
                provider.observations[0].world.external_market.has_value());
            EXPECT_EQ(
                provider.observations[0].world.external_market->symbol,
                "ALPHA_426USDT");
            EXPECT_FALSE(
                provider.observations[1].world.external_market.has_value());
            EXPECT_FALSE(
                provider.observations[2].world.external_market.has_value());
            EXPECT_EQ(
                feed.requested_at,
                (std::vector<std::int64_t>{1'020, 7'000, 8'000}));
            ASSERT_EQ(runtime.trace().size(), 3U);
            EXPECT_EQ(runtime.trace()[0].status, AgentTurnStatus::Held);
            EXPECT_EQ(runtime.trace()[0].local_timestamp_ms, 1'020);
            ASSERT_TRUE(
                runtime.trace()[0].external_market_context.has_value());
            EXPECT_EQ(
                runtime.trace()[0].external_market_context->freshness,
                ExternalMarketFreshness::Fresh);
            EXPECT_EQ(
                runtime.trace()[0].external_market_context->symbol,
                "ALPHA_426USDT");
            EXPECT_EQ(
                runtime.trace()[0].external_market_context
                    ->latest_trade_price,
                (ExternalPrice{4'070'000, 8}));
            EXPECT_EQ(
                runtime.trace()[0].external_market_context->best_bid,
                (ExternalPrice{4'060'000, 8}));
            EXPECT_EQ(
                runtime.trace()[0].external_market_context->best_ask,
                (ExternalPrice{4'080'000, 8}));
            EXPECT_EQ(
                runtime.trace()[0].external_market_context
                    ->event_timestamp_ms,
                1'000);
            EXPECT_EQ(
                runtime.trace()[0].external_market_context
                    ->local_receive_timestamp_ms,
                1'010);
            ASSERT_TRUE(
                runtime.trace()[1].external_market_context.has_value());
            EXPECT_EQ(
                runtime.trace()[1].external_market_context->freshness,
                ExternalMarketFreshness::Stale);
            ASSERT_TRUE(
                runtime.trace()[2].external_market_context.has_value());
            EXPECT_EQ(
                runtime.trace()[2].external_market_context->freshness,
                ExternalMarketFreshness::Unavailable);
            const PerAgentExperimentMetrics* metrics =
                runtime.metrics().find_agent(agent_a);
            ASSERT_NE(metrics, nullptr);
            EXPECT_EQ(metrics->turns, 3U);
            EXPECT_EQ(metrics->holds, 3U);
            EXPECT_EQ(metrics->successful_executions, 0U);
            EXPECT_EQ(metrics->zero_utility_turns, 3U);
            EXPECT_EQ(metrics->cumulative_utility_delta, 0);
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }
    }  // namespace
}  // namespace exchange
