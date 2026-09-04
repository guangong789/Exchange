#include "exchange/deepseek_adapter.hpp"
#include "exchange/model_agent_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "exchange/agent_action_gateway.hpp"
#include "exchange/agent_observation_service.hpp"
#include "exchange/arena.hpp"

namespace exchange {
    namespace {
        constexpr InstrumentContext test_instrument{20, 10, 1, 1, 1};
        constexpr AgentId model_agent_id = 101;
        constexpr AgentId counterparty_agent_id = 202;
        constexpr AccountId model_account_id = 1;
        constexpr AccountId counterparty_account_id = 2;

        class FixedSellPolicy final : public AgentPolicy {
        public:
            AgentAction decide(const AgentObservation&) const override {
                return SubmitOrderAction{Side::Sell, 100, 2};
            }
        };

        struct LiveWorld {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator execution_coordinator{
                test_instrument,
                accounts,
                reservations,
                matching_engine,
                events,
                ledger};
            AgentRegistry registry;
            AgentObservationService observations{
                registry,
                accounts,
                matching_engine.order_book(),
                test_instrument};
            AgentActionGateway actions{registry, execution_coordinator};
        };

        class DeepSeekLiveTest : public ::testing::Test {
        protected:
            void SetUp() override {
                if (std::getenv("DEEPSEEK_API_KEY") == nullptr) {
                    GTEST_SKIP() << "DEEPSEEK_API_KEY is not set";
                }
            }
        };

        void create_agent(
            LiveWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent({agent_id, account_id}));
        }

        const char* action_name(const AgentAction& action) {
            return std::visit(
                [](const auto& payload) {
                    using Action = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
                        return payload.side == Side::Buy
                            ? "SUBMIT_BUY"
                            : "SUBMIT_SELL";
                    } else if constexpr (std::is_same_v<
                                             Action,
                                             CancelOrderAction>) {
                        return "CANCEL";
                    } else {
                        return "HOLD";
                    }
                },
                action);
        }

        const char* result_name(const AgentActionResult& result) {
            return std::visit(
                [](const auto& payload) {
                    using Result = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<Result, SubmitActionResult>) {
                        switch (payload.result) {
                            case SubmitResult::Accepted:
                                return "ACCEPTED";
                            case SubmitResult::InsufficientFunds:
                                return "INSUFFICIENT_FUNDS";
                            case SubmitResult::DuplicateOrder:
                                return "DUPLICATE_ORDER";
                            case SubmitResult::InvalidOrder:
                                return "INVALID_ORDER";
                            case SubmitResult::CounterpartyNotAccountBacked:
                                return "COUNTERPARTY_NOT_ACCOUNT_BACKED";
                        }
                    } else if constexpr (std::is_same_v<
                                             Result,
                                             CancelActionResult>) {
                        switch (payload.result) {
                            case CancelResult::Cancelled:
                                return "CANCELLED";
                            case CancelResult::NotFound:
                                return "NOT_FOUND";
                            case CancelResult::NotOwner:
                                return "NOT_OWNER";
                        }
                    } else {
                        return "HELD";
                    }
                    return "UNKNOWN";
                },
                result);
        }

        Amount total_holding(
            const AccountStore& accounts,
            AccountId account_id,
            AssetId asset_id) {
            const auto balance = accounts.find_balance(account_id, asset_id);
            return balance.has_value()
                ? balance->available + balance->reserved
                : 0;
        }

        bool ledger_contains_trade(const Ledger& ledger) {
            for (const LedgerEntry& entry : ledger.entries()) {
                if (std::holds_alternative<TradeLedgerMetadata>(
                        entry.transaction.metadata)) {
                    return true;
                }
            }
            return false;
        }

        void print_probe(
            std::string_view probe,
            const ArenaTurnRecord& turn,
            const ObjectiveProgress& outcome) {
            std::cout << "probe=" << probe
                      << " action=" << action_name(turn.action)
                      << " result=" << result_name(turn.result)
                      << " objective_before="
                      << (turn.observation.objective->achieved
                              ? "true"
                              : "false")
                      << " current_before="
                      << turn.observation.objective->current_amount
                      << " current_after=" << outcome.current_amount
                      << " achieved_after="
                      << (outcome.achieved ? "true" : "false") << '\n';
        }
    }  // namespace

    TEST_F(DeepSeekLiveTest, AcquiresTargetThenHolds) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        create_agent(world, counterparty_agent_id, counterparty_account_id);
        world.accounts.fund(model_account_id, 10, 600);
        world.accounts.fund(counterparty_account_id, 20, 6);

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        FixedSellPolicy seller_policy;
        Arena arena(
            ArenaScenario{
                {3},
                {
                    {counterparty_agent_id, {10, 400}},
                    {model_agent_id, {20, 4}},
                }},
            {
                {counterparty_agent_id, &seller_policy},
                {model_agent_id, &model_policy},
            },
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 6U);
        bool acted_before_achievement = false;
        bool held_after_achievement = false;
        for (const std::size_t index : {
                 std::size_t{1},
                 std::size_t{3},
                 std::size_t{5}}) {
            const ArenaTurnRecord& turn = arena.trace()[index];
            ASSERT_TRUE(turn.observation.objective.has_value());
            if (turn.observation.objective->achieved) {
                EXPECT_TRUE(std::holds_alternative<HoldAction>(turn.action));
                held_after_achievement =
                    held_after_achievement
                    || std::holds_alternative<HoldAction>(turn.action);
            } else {
                ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(
                    turn.action));
                EXPECT_EQ(
                    std::get<SubmitOrderAction>(turn.action).side,
                    Side::Buy);
                acted_before_achievement = true;
            }
            std::cout << "probe=acquire_target round=" << turn.round
                      << " action=" << action_name(turn.action)
                      << " result=" << result_name(turn.result)
                      << " current_before="
                      << turn.observation.objective->current_amount << '\n';
        }

        ASSERT_EQ(arena.outcomes().size(), 2U);
        const ObjectiveProgress& outcome = arena.outcomes()[1].objective;
        EXPECT_TRUE(acted_before_achievement);
        EXPECT_TRUE(held_after_achievement);
        EXPECT_TRUE(outcome.achieved);
        EXPECT_GE(outcome.current_amount, outcome.target_amount);
        EXPECT_TRUE(ledger_contains_trade(world.ledger));
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 20)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    20),
            6);
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 10)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    10),
            600);
        std::cout << "probe=acquire_target terminal_holding="
                  << outcome.current_amount
                  << " target=" << outcome.target_amount
                  << " achieved=true\n";
    }

    TEST_F(DeepSeekLiveTest, HoldsWhenObjectiveAlreadyAchieved) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        create_agent(world, counterparty_agent_id, counterparty_account_id);
        world.accounts.fund(model_account_id, 20, 4);
        world.accounts.fund(model_account_id, 10, 100);
        world.accounts.fund(counterparty_account_id, 20, 1);
        ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(
            world.actions.execute(
                counterparty_agent_id,
                SubmitOrderAction{Side::Sell, 100, 1})));
        const auto base_before = world.accounts.find_balance(
            model_account_id,
            20);
        const auto quote_before = world.accounts.find_balance(
            model_account_id,
            10);
        const std::size_t ledger_size_before = world.ledger.entries().size();
        const std::size_t order_count_before =
            world.matching_engine.order_book().order_count();

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        Arena arena(
            ArenaScenario{{1}, {{model_agent_id, {20, 4}}}},
            {{model_agent_id, &model_policy}},
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 1U);
        const ArenaTurnRecord& turn = arena.trace()[0];
        ASSERT_TRUE(turn.observation.objective.has_value());
        EXPECT_TRUE(turn.observation.objective->achieved);
        EXPECT_TRUE(std::holds_alternative<HoldAction>(turn.action));
        EXPECT_TRUE(std::holds_alternative<HoldActionResult>(turn.result));
        EXPECT_EQ(world.accounts.find_balance(model_account_id, 20),
                  base_before);
        EXPECT_EQ(world.accounts.find_balance(model_account_id, 10),
                  quote_before);
        EXPECT_EQ(world.ledger.entries().size(), ledger_size_before);
        EXPECT_EQ(world.matching_engine.order_book().order_count(),
                  order_count_before);
        print_probe("already_achieved", turn, arena.outcomes()[0].objective);
    }

    TEST_F(DeepSeekLiveTest, RespectsMissingAsk) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        world.accounts.fund(model_account_id, 10, 500);

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        Arena arena(
            ArenaScenario{{1}, {{model_agent_id, {20, 2}}}},
            {{model_agent_id, &model_policy}},
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 1U);
        const ArenaTurnRecord& turn = arena.trace()[0];
        EXPECT_FALSE(turn.observation.best_ask.has_value());
        if (std::holds_alternative<HoldAction>(turn.action)) {
            EXPECT_TRUE(std::holds_alternative<HoldActionResult>(turn.result));
        } else {
            ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
            EXPECT_EQ(std::get<SubmitOrderAction>(turn.action).side, Side::Buy);
            ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(turn.result));
            EXPECT_EQ(
                std::get<SubmitActionResult>(turn.result).result,
                SubmitResult::Accepted);
        }
        EXPECT_FALSE(ledger_contains_trade(world.ledger));
        EXPECT_EQ(total_holding(world.accounts, model_account_id, 20), 0);
        print_probe("missing_ask", turn, arena.outcomes()[0].objective);
    }

    TEST_F(DeepSeekLiveTest, SellsBaseToAcquireQuote) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        create_agent(world, counterparty_agent_id, counterparty_account_id);
        world.accounts.fund(model_account_id, 20, 2);
        world.accounts.fund(counterparty_account_id, 10, 200);
        const AgentActionResult resting_buy = world.actions.execute(
            counterparty_agent_id,
            SubmitOrderAction{Side::Buy, 100, 2});
        ASSERT_EQ(
            std::get<SubmitActionResult>(resting_buy).result,
            SubmitResult::Accepted);

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        Arena arena(
            ArenaScenario{{1}, {{model_agent_id, {10, 100}}}},
            {{model_agent_id, &model_policy}},
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 1U);
        const ArenaTurnRecord& turn = arena.trace()[0];
        ASSERT_EQ(turn.observation.best_bid, 100);
        ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
        EXPECT_EQ(std::get<SubmitOrderAction>(turn.action).side, Side::Sell);
        ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(turn.result));
        EXPECT_EQ(
            std::get<SubmitActionResult>(turn.result).result,
            SubmitResult::Accepted);
        EXPECT_GT(total_holding(world.accounts, model_account_id, 10), 0);
        EXPECT_TRUE(ledger_contains_trade(world.ledger));
        EXPECT_TRUE(arena.outcomes()[0].objective.achieved);
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 20)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    20),
            2);
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 10)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    10),
            200);
        print_probe("sell_for_quote", turn, arena.outcomes()[0].objective);
    }

    TEST_F(DeepSeekLiveTest, UnderstandsReservedBalanceCountsTowardObjective) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        world.accounts.fund(model_account_id, 20, 4);
        world.accounts.fund(model_account_id, 10, 400);
        const AgentActionResult resting_sell = world.actions.execute(
            model_agent_id,
            SubmitOrderAction{Side::Sell, 200, 3});
        ASSERT_EQ(
            std::get<SubmitActionResult>(resting_sell).result,
            SubmitResult::Accepted);
        ASSERT_EQ(
            world.accounts.find_balance(model_account_id, 20),
            (Balance{1, 3}));
        const auto base_before = world.accounts.find_balance(
            model_account_id,
            20);
        const auto quote_before = world.accounts.find_balance(
            model_account_id,
            10);
        const std::size_t ledger_size_before = world.ledger.entries().size();
        const std::size_t order_count_before =
            world.matching_engine.order_book().order_count();

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        Arena arena(
            ArenaScenario{{1}, {{model_agent_id, {20, 4}}}},
            {{model_agent_id, &model_policy}},
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 1U);
        const ArenaTurnRecord& turn = arena.trace()[0];
        ASSERT_TRUE(turn.observation.objective.has_value());
        EXPECT_EQ(turn.observation.objective->current_amount, 4);
        EXPECT_TRUE(turn.observation.objective->achieved);
        EXPECT_TRUE(std::holds_alternative<HoldAction>(turn.action));
        EXPECT_EQ(world.accounts.find_balance(model_account_id, 20),
                  base_before);
        EXPECT_EQ(world.accounts.find_balance(model_account_id, 10),
                  quote_before);
        EXPECT_EQ(world.ledger.entries().size(), ledger_size_before);
        EXPECT_EQ(world.matching_engine.order_book().order_count(),
                  order_count_before);
        print_probe("reserved_counts", turn, arena.outcomes()[0].objective);
    }

    TEST_F(DeepSeekLiveTest, RespectsLimitedBudget) {
        LiveWorld world;
        create_agent(world, model_agent_id, model_account_id);
        create_agent(world, counterparty_agent_id, counterparty_account_id);
        world.accounts.fund(model_account_id, 10, 200);
        world.accounts.fund(counterparty_account_id, 20, 10);
        const AgentActionResult resting_sell = world.actions.execute(
            counterparty_agent_id,
            SubmitOrderAction{Side::Sell, 100, 10});
        ASSERT_EQ(
            std::get<SubmitActionResult>(resting_sell).result,
            SubmitResult::Accepted);

        DeepSeekAdapter adapter;
        ModelAgentPolicy model_policy(adapter);
        Arena arena(
            ArenaScenario{{1}, {{model_agent_id, {20, 10}}}},
            {{model_agent_id, &model_policy}},
            world.registry,
            world.observations,
            world.actions);

        arena.run();

        ASSERT_EQ(arena.trace().size(), 1U);
        const ArenaTurnRecord& turn = arena.trace()[0];
        std::string_view classification;
        if (std::holds_alternative<HoldAction>(turn.action)) {
            EXPECT_TRUE(std::holds_alternative<HoldActionResult>(turn.result));
            classification = "held";
        } else {
            ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
            EXPECT_EQ(std::get<SubmitOrderAction>(turn.action).side, Side::Buy);
            ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(turn.result));
            const SubmitResult result =
                std::get<SubmitActionResult>(turn.result).result;
            if (result == SubmitResult::InsufficientFunds) {
                classification = "rejected_insufficient_funds";
            } else {
                EXPECT_EQ(result, SubmitResult::Accepted);
                classification =
                    total_holding(world.accounts, model_account_id, 20) > 0
                    ? "affordable_progress"
                    : "accepted_resting_bid";
            }
        }
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 20)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    20),
            10);
        EXPECT_EQ(
            total_holding(world.accounts, model_account_id, 10)
                + total_holding(
                    world.accounts,
                    counterparty_account_id,
                    10),
            200);
        EXPECT_FALSE(arena.outcomes()[0].objective.achieved);
        print_probe("limited_budget", turn, arena.outcomes()[0].objective);
        std::cout << "probe=limited_budget classification="
                  << classification << '\n';
    }
}  // namespace exchange
