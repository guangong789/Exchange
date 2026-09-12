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
            AgentObservationService observations{
                registry,
                accounts,
                reservations,
                matching_engine.order_book(),
                instrument};
            TradingRequestAgentExecutionAdapter execution{
                registry,
                request_executor};
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
                 {agent_b, &buyer, std::nullopt}},
                world.observations,
                world.execution,
                instrument);

            runtime.run_step();

            ASSERT_EQ(runtime.trace().size(), 2U);
            EXPECT_EQ(runtime.current_step(), 1U);
            EXPECT_EQ(runtime.trace()[0].status, AgentTurnStatus::Executed);
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
                AgentTurnStatus::ActionRejected);
            EXPECT_EQ(
                runtime.trace()[0].validation,
                AgentActionValidationResult::InvalidPrice);
            EXPECT_FALSE(runtime.trace()[0].execution_result.has_value());
            EXPECT_EQ(world.accounts.find_balance(account_a, 10), before);
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
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
            EXPECT_FALSE(runtime.trace()[0].execution_result.has_value());
            EXPECT_EQ(world.accounts.find_balance(account_a, 10), before);
            EXPECT_TRUE(world.reservations.entries().empty());
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_TRUE(world.events.empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
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
                {{agent_a, &provider, std::nullopt}},
                world.observations,
                world.execution,
                instrument,
                &feed);

            runtime.run_step_at(1'020);
            feed.state.freshness = ExternalMarketFreshness::Stale;
            runtime.run_step_at(7'000);

            ASSERT_EQ(provider.observations.size(), 2U);
            ASSERT_TRUE(
                provider.observations[0].world.external_market.has_value());
            EXPECT_EQ(
                provider.observations[0].world.external_market->symbol,
                "ALPHA_426USDT");
            EXPECT_FALSE(
                provider.observations[1].world.external_market.has_value());
            EXPECT_EQ(
                feed.requested_at,
                (std::vector<std::int64_t>{1'020, 7'000}));
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }
    }  // namespace
}  // namespace exchange
