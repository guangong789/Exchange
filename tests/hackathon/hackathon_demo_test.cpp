#include "exchange/hackathon/hackathon_demo.hpp"
#include "exchange/accounting/funding_coordinator.hpp"

#include <optional>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr InstrumentContext test_instrument{
            20,
            10,
            1,
            1,
            1,
        };
        constexpr AgentId trader_agent = 101;
        constexpr AgentId analyst_agent = 202;
        constexpr AgentId risk_agent_id = 303;
        constexpr AccountId trader_account = 1;
        constexpr AccountId analyst_account = 2;
        constexpr AccountId risk_account = 3;

        struct DemoWorld {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator execution{
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
            AgentActionGateway actions{registry, execution};
        };

        class SignalDrivenBuyPolicy final : public PremiumSignalPolicy {
        public:
            explicit SignalDrivenBuyPolicy(
                std::optional<Quantity> requested_quantity = std::nullopt)
                : requested_quantity_(requested_quantity) {}

            AgentAction decide(
                const AgentObservation& observation,
                const PremiumMarketSignal& premium_signal,
                const RiskGuidance& risk_guidance) const override {
                observed_observation_ = observation;
                observed_signal_ = premium_signal;
                observed_risk_guidance_ = risk_guidance;
                return SubmitOrderAction{
                    Side::Buy,
                    100,
                    requested_quantity_.value_or(
                        risk_guidance.max_recommended_quantity)};
            }

            [[nodiscard]] const std::optional<PremiumMarketSignal>&
            observed_signal() const noexcept {
                return observed_signal_;
            }

            [[nodiscard]] const std::optional<RiskGuidance>&
            observed_risk_guidance() const noexcept {
                return observed_risk_guidance_;
            }

        private:
            std::optional<Quantity> requested_quantity_;
            mutable std::optional<AgentObservation> observed_observation_;
            mutable std::optional<PremiumMarketSignal> observed_signal_;
            mutable std::optional<RiskGuidance> observed_risk_guidance_;
        };

        class CapturingModelAdapter final : public ModelAdapter {
        public:
            ModelResponse invoke(const ModelRequest& request) override {
                last_request = request;
                ++calls;
                return ModelResponse{
                    R"({"action":"submit_order","side":"buy","price":100,"quantity":1})"};
            }

            std::optional<ModelRequest> last_request;
            std::size_t calls{};
        };

        class SequencedModelAdapter final : public ModelAdapter {
        public:
            explicit SequencedModelAdapter(std::vector<std::string> responses)
                : responses_(std::move(responses)) {}

            ModelResponse invoke(const ModelRequest& request) override {
                requests.push_back(request);
                if (next_ >= responses_.size()) {
                    throw std::logic_error("unexpected model invocation");
                }
                return ModelResponse{responses_[next_++]};
            }

            std::vector<ModelRequest> requests;

        private:
            std::vector<std::string> responses_;
            std::size_t next_{};
        };

        ExternalPaymentRequirement payment_requirement() {
            return ExternalPaymentRequirement{
                2,
                "http://127.0.0.1/premium-signal",
                "Premium Market Signal",
                "application/json",
                "exact",
                "eip155:97",
                "10000",
                "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                "0x1111111111111111111111111111111111111111",
                60,
            };
        }

        PremiumMarketSignal premium_signal() {
            return PremiumMarketSignal{
                "BUY_BASE",
                95,
                "Deterministic premium analyst signal",
            };
        }

        PremiumInformationService premium_service() {
            return PremiumInformationService(
                payment_requirement(), premium_signal());
        }

        HackathonDemoConfig demo_config() {
            return HackathonDemoConfig{
                trader_agent,
                analyst_agent,
                risk_agent_id,
                AssetTargetObjective{20, 1},
            };
        }

        RiskGuidance risk_guidance() {
            return DeterministicRiskAgent(2'000).assess(AgentObservation{
                trader_agent,
                trader_account,
                90,
                100,
                std::nullopt,
                Balance{500, 0},
                ObjectiveProgress{trader_agent, 20, 0, 1, false},
                std::nullopt,
            });
        }

        void create_agent(
            DemoWorld& world,
            AgentId agent_id,
            AccountId account_id) {
            ASSERT_TRUE(world.accounts.create_account(account_id));
            ASSERT_TRUE(world.registry.register_agent(
                AgentIdentity{agent_id, account_id}));
        }

        ExternalPaymentPreview approved_preview() {
            return ExternalPaymentPreview{
                ExternalPaymentPreviewStatus::Approved,
                "deterministic-fake-preview",
                "eip155:97",
                "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                "10000",
                "READY_TO_SIGN",
                {},
            };
        }

        AgentObservation risk_observation(
            Amount available_quote,
            std::optional<Price> best_ask,
            Amount current_amount,
            Amount target_amount) {
            return AgentObservation{
                trader_agent,
                trader_account,
                std::nullopt,
                best_ask,
                std::nullopt,
                Balance{available_quote, 0},
                ObjectiveProgress{
                    trader_agent,
                    20,
                    current_amount,
                    target_amount,
                    current_amount >= target_amount},
                std::nullopt,
            };
        }

        TEST(DeterministicRiskAgentTest,
             CalculatesNormalScenarioFromObservedState) {
            const RiskGuidance guidance = DeterministicRiskAgent(2'000).assess(
                risk_observation(500, 100, 0, 1));

            EXPECT_EQ(guidance.available_quote, 500);
            EXPECT_EQ(guidance.local_best_ask, 100);
            EXPECT_EQ(guidance.risk_budget_bps, 2'000);
            EXPECT_EQ(guidance.affordable_quantity, 5);
            EXPECT_EQ(guidance.risk_budget_quantity, 1);
            EXPECT_EQ(guidance.objective_remaining, 1);
            EXPECT_EQ(guidance.max_recommended_quantity, 1);
        }

        TEST(DeterministicRiskAgentTest,
             FullBudgetMakesAffordabilityAndBudgetBindTogether) {
            const RiskGuidance guidance = DeterministicRiskAgent(10'000).assess(
                risk_observation(250, 100, 0, 10));

            EXPECT_EQ(guidance.affordable_quantity, 2);
            EXPECT_EQ(guidance.risk_budget_quantity, 2);
            EXPECT_EQ(guidance.objective_remaining, 10);
            EXPECT_EQ(guidance.max_recommended_quantity, 2);
        }

        TEST(DeterministicRiskAgentTest, RiskBudgetCanBindRecommendation) {
            const RiskGuidance guidance = DeterministicRiskAgent(2'000).assess(
                risk_observation(500, 100, 0, 10));

            EXPECT_EQ(guidance.affordable_quantity, 5);
            EXPECT_EQ(guidance.risk_budget_quantity, 1);
            EXPECT_EQ(guidance.objective_remaining, 10);
            EXPECT_EQ(guidance.max_recommended_quantity, 1);
        }

        TEST(DeterministicRiskAgentTest, ObjectiveRemainingCanBindRecommendation) {
            const RiskGuidance guidance = DeterministicRiskAgent(10'000).assess(
                risk_observation(500, 100, 0, 1));

            EXPECT_EQ(guidance.affordable_quantity, 5);
            EXPECT_EQ(guidance.risk_budget_quantity, 5);
            EXPECT_EQ(guidance.objective_remaining, 1);
            EXPECT_EQ(guidance.max_recommended_quantity, 1);
        }

        TEST(DeterministicRiskAgentTest, AchievedObjectiveProducesZeroRecommendation) {
            const RiskGuidance guidance = DeterministicRiskAgent(2'000).assess(
                risk_observation(500, 100, 1, 1));

            EXPECT_EQ(guidance.objective_remaining, 0);
            EXPECT_EQ(guidance.max_recommended_quantity, 0);
        }

        TEST(DeterministicRiskAgentTest, MissingOrInvalidAskIsConservative) {
            const DeterministicRiskAgent agent(2'000);
            for (const std::optional<Price> ask : {
                     std::optional<Price>{},
                     std::optional<Price>{0},
                     std::optional<Price>{-1},
                 }) {
                const RiskGuidance guidance = agent.assess(
                    risk_observation(500, ask, 0, 1));
                EXPECT_EQ(guidance.affordable_quantity, 0);
                EXPECT_EQ(guidance.risk_budget_quantity, 0);
                EXPECT_EQ(guidance.max_recommended_quantity, 0);
            }
        }

        TEST(DeterministicRiskAgentTest, RejectsRiskBudgetAboveOneHundredPercent) {
            EXPECT_THROW(DeterministicRiskAgent(10'001), std::invalid_argument);
        }

        TEST(DeterministicRiskAgentTest, ZeroRiskBudgetIsValidAndConservative) {
            const RiskGuidance guidance = DeterministicRiskAgent(0).assess(
                risk_observation(500, 100, 0, 1));

            EXPECT_EQ(guidance.risk_budget_quantity, 0);
            EXPECT_EQ(guidance.max_recommended_quantity, 0);
        }

        TEST(DeterministicRiskAgentTest, UsesWideIntermediateForRiskBudgetMath) {
            const Amount maximum = std::numeric_limits<Amount>::max();
            const RiskGuidance guidance = DeterministicRiskAgent(10'000).assess(
                risk_observation(maximum, 1, 0, maximum));

            EXPECT_EQ(guidance.affordable_quantity, maximum);
            EXPECT_EQ(guidance.risk_budget_quantity, maximum);
            EXPECT_EQ(guidance.objective_remaining, maximum);
            EXPECT_EQ(guidance.max_recommended_quantity, maximum);
        }

        TEST(HackathonModelPremiumSignalPolicyTest,
             SendsObservationAnalystAndRiskContextThroughStrictModelPath) {
            CapturingModelAdapter adapter;
            ModelAgentPolicy model_policy(adapter);
            ModelPremiumSignalPolicy policy(model_policy);
            const AgentObservation observation{
                trader_agent,
                trader_account,
                90,
                100,
                std::nullopt,
                Balance{500, 0},
                ObjectiveProgress{trader_agent, 20, 0, 1, false},
                ExternalMarketSnapshot{"BTCUSDT", 6'000'000, 6'000'100},
            };

            EXPECT_EQ(
                policy.decide(
                    observation, premium_signal(), risk_guidance()),
                (AgentAction{SubmitOrderAction{Side::Buy, 100, 1}}));
            ASSERT_EQ(adapter.calls, 1U);
            ASSERT_TRUE(adapter.last_request.has_value());
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "target asset=20, current holding=0, target holding=1"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "Analyst premium signal: BUY_BASE"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "Analyst confidence: 95%"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "affordable_qty = 5"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "risk_budget_qty = 1"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "objective_remaining = 1"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "recommended_max_qty = 1"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          "External market: symbol=BTCUSDT, best bid=6000000, "
                          "best ask=6000100"),
                      std::string::npos);
            EXPECT_NE(adapter.last_request->user_prompt.find(
                          risk_guidance().reason),
                      std::string::npos);
        }

        TEST(HackathonDemoTest,
             ApprovedPreviewGatesSignalIntoCognitionAndExecutesExistingAction) {
            DemoWorld world;
            create_agent(world, trader_agent, trader_account);
            create_agent(world, analyst_agent, analyst_account);
            create_agent(world, risk_agent_id, risk_account);
            world.accounts.fund(trader_account, 10, 500);
            world.accounts.fund(analyst_account, 20, 2);

            ASSERT_EQ(
                world.actions.execute(
                    analyst_agent,
                    SubmitOrderAction{Side::Sell, 100, 1}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));

            SignalDrivenBuyPolicy trader_policy;
            DeterministicRiskAgent risk_agent(2'000);
            HackathonDemoScenario scenario(
                demo_config(),
                premium_service(),
                world.registry,
                world.observations,
                world.actions,
                risk_agent,
                trader_policy);
            std::optional<ExternalPaymentRequirement> received_requirement;

            const HackathonDemoTurn turn = scenario.run_once(
                [&received_requirement](const ExternalPaymentRequirement& requirement) {
                    received_requirement = requirement;
                    return approved_preview();
                });

            EXPECT_EQ(received_requirement, payment_requirement());
            EXPECT_TRUE(turn.preview_authorized);
            EXPECT_EQ(turn.premium_signal, premium_signal());
            EXPECT_EQ(trader_policy.observed_signal(), premium_signal());
            EXPECT_EQ(turn.risk_guidance, risk_guidance());
            EXPECT_EQ(
                trader_policy.observed_risk_guidance(), risk_guidance());
            EXPECT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
            EXPECT_EQ(
                turn.action_result,
                (AgentActionResult{SubmitActionResult{
                    2,
                    2,
                    SubmitResult::Accepted}}));
            EXPECT_TRUE(turn.trader_objective.achieved);

            EXPECT_EQ(world.accounts.find_balance(trader_account, 10),
                      (Balance{400, 0}));
            EXPECT_EQ(world.accounts.find_balance(trader_account, 20),
                      (Balance{1, 0}));
            EXPECT_EQ(world.accounts.find_balance(analyst_account, 20),
                      (Balance{1, 0}));
            EXPECT_EQ(world.accounts.find_balance(analyst_account, 10),
                      (Balance{100, 0}));
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
            ASSERT_EQ(world.ledger.entries().size(), 3U);
            EXPECT_TRUE(std::holds_alternative<TradeLedgerMetadata>(
                world.ledger.entries().back().transaction.metadata));
        }

        TEST(HackathonDemoTest,
             RejectedPreviewExposesNoSignalAndLeavesEconomicStateUnchanged) {
            DemoWorld world;
            create_agent(world, trader_agent, trader_account);
            create_agent(world, analyst_agent, analyst_account);
            create_agent(world, risk_agent_id, risk_account);
            world.accounts.fund(trader_account, 10, 500);
            const auto balance_before = world.accounts.find_balance(
                trader_account, 10);

            SignalDrivenBuyPolicy trader_policy;
            DeterministicRiskAgent risk_agent(2'000);
            HackathonDemoScenario scenario(
                demo_config(),
                premium_service(),
                world.registry,
                world.observations,
                world.actions,
                risk_agent,
                trader_policy);

            const HackathonDemoTurn turn = scenario.run_once(
                [](const ExternalPaymentRequirement&) {
                    return ExternalPaymentPreview{
                        ExternalPaymentPreviewStatus::Rejected,
                        "deterministic-fake-preview",
                        "eip155:97",
                        "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                        "10000",
                        "ACTION_REQUIRED",
                        {"INSUFFICIENT_BALANCE"},
                    };
                });

            EXPECT_FALSE(turn.preview_authorized);
            EXPECT_FALSE(turn.premium_signal.has_value());
            EXPECT_FALSE(trader_policy.observed_signal().has_value());
            EXPECT_FALSE(
                trader_policy.observed_risk_guidance().has_value());
            EXPECT_TRUE(std::holds_alternative<HoldAction>(turn.action));
            EXPECT_TRUE(std::holds_alternative<HoldActionResult>(turn.action_result));
            EXPECT_EQ(world.accounts.find_balance(trader_account, 10),
                      balance_before);
            EXPECT_TRUE(world.ledger.entries().empty());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 0U);
        }

        TEST(HackathonDemoReplayTest,
             CapturedEconomicEvidenceRebuildsTheSameFreshFinancialWorld) {
            DemoWorld original;
            create_agent(original, trader_agent, trader_account);
            create_agent(original, analyst_agent, analyst_account);
            create_agent(original, risk_agent_id, risk_account);
            original.accounts.fund(trader_account, 10, 500);
            original.accounts.fund(analyst_account, 20, 2);
            ASSERT_TRUE(std::holds_alternative<SubmitActionResult>(
                original.actions.execute(
                    analyst_agent,
                    SubmitOrderAction{Side::Sell, 100, 1})));

            SignalDrivenBuyPolicy original_policy;
            DeterministicRiskAgent original_risk_agent(2'000);
            HackathonDemoScenario original_scenario(
                demo_config(),
                premium_service(),
                original.registry,
                original.observations,
                original.actions,
                original_risk_agent,
                original_policy);
            const HackathonDemoTurn original_turn = original_scenario.run_once(
                [](const ExternalPaymentRequirement&) {
                    return approved_preview();
                });
            const HackathonEconomicReplayEvidence evidence =
                capture_hackathon_economic_replay_evidence(
                    original.registry,
                    trader_agent,
                    original_turn.action,
                    original_turn.action_result);

            DemoWorld replay;
            create_agent(replay, trader_agent, trader_account);
            create_agent(replay, analyst_agent, analyst_account);
            create_agent(replay, risk_agent_id, risk_account);
            replay.accounts.fund(trader_account, 10, 500);
            replay.accounts.fund(analyst_account, 20, 2);
            ASSERT_EQ(
                replay.actions.execute(
                    analyst_agent,
                    SubmitOrderAction{Side::Sell, 100, 1}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));

            EXPECT_EQ(evidence.account_id, trader_account);
            EXPECT_EQ(evidence.order.id, 2U);
            EXPECT_EQ(evidence.order.side, Side::Buy);
            EXPECT_EQ(evidence.order.type, OrderType::Limit);
            EXPECT_EQ(evidence.order.price, 100);
            EXPECT_EQ(evidence.order.quantity, 1);
            EXPECT_EQ(evidence.order.timestamp, 2U);
            EXPECT_EQ(
                replay_hackathon_economic_evidence(evidence, replay.execution),
                evidence.expected_result);

            EXPECT_EQ(replay.accounts.find_balance(trader_account, 10),
                      original.accounts.find_balance(trader_account, 10));
            EXPECT_EQ(replay.accounts.find_balance(trader_account, 20),
                      original.accounts.find_balance(trader_account, 20));
            EXPECT_EQ(replay.accounts.find_balance(analyst_account, 10),
                      original.accounts.find_balance(analyst_account, 10));
            EXPECT_EQ(replay.accounts.find_balance(analyst_account, 20),
                      original.accounts.find_balance(analyst_account, 20));
            EXPECT_EQ(replay.matching_engine.order_book().order_count(),
                      original.matching_engine.order_book().order_count());
            EXPECT_EQ(replay.ledger.entries(), original.ledger.entries());

            const ObjectiveProgress replay_objective = *replay.observations
                .observe(trader_agent, demo_config().trader_objective)
                .objective;
            EXPECT_EQ(replay_objective, original_turn.trader_objective);
            EXPECT_TRUE(replay_objective.achieved);
        }

        TEST(HackathonDemoRiskTest,
             TraderCanIgnoreAdviceButCoreRejectsUnaffordableActionSafely) {
            DemoWorld world;
            create_agent(world, trader_agent, trader_account);
            create_agent(world, analyst_agent, analyst_account);
            create_agent(world, risk_agent_id, risk_account);
            world.accounts.fund(trader_account, 10, 500);
            world.accounts.fund(analyst_account, 20, 2);
            ASSERT_EQ(
                world.actions.execute(
                    analyst_agent,
                    SubmitOrderAction{Side::Sell, 100, 1}),
                (AgentActionResult{SubmitActionResult{
                    1,
                    1,
                    SubmitResult::Accepted}}));

            const auto trader_quote_before = world.accounts.find_balance(
                trader_account, 10);
            const auto trader_base_before = world.accounts.find_balance(
                trader_account, 20);
            const auto analyst_quote_before = world.accounts.find_balance(
                analyst_account, 10);
            const auto analyst_base_before = world.accounts.find_balance(
                analyst_account, 20);
            const std::vector<LedgerEntry> ledger_before =
                world.ledger.entries();
            const auto reservation_before = world.reservations.find(1);

            SignalDrivenBuyPolicy oversized_policy(Quantity{10});
            DeterministicRiskAgent risk_agent(2'000);
            HackathonDemoScenario scenario(
                demo_config(),
                premium_service(),
                world.registry,
                world.observations,
                world.actions,
                risk_agent,
                oversized_policy);
            const HackathonDemoTurn turn = scenario.run_once(
                [](const ExternalPaymentRequirement&) {
                    return approved_preview();
                });

            EXPECT_EQ(turn.risk_guidance.max_recommended_quantity, 1);
            ASSERT_TRUE(std::holds_alternative<SubmitOrderAction>(turn.action));
            EXPECT_EQ(std::get<SubmitOrderAction>(turn.action).quantity, 10);
            EXPECT_EQ(
                turn.action_result,
                (AgentActionResult{SubmitActionResult{
                    2,
                    2,
                    SubmitResult::InsufficientFunds}}));
            EXPECT_FALSE(turn.trader_objective.achieved);

            EXPECT_EQ(world.accounts.find_balance(trader_account, 10),
                      trader_quote_before);
            EXPECT_EQ(world.accounts.find_balance(trader_account, 20),
                      trader_base_before);
            EXPECT_EQ(world.accounts.find_balance(analyst_account, 10),
                      analyst_quote_before);
            EXPECT_EQ(world.accounts.find_balance(analyst_account, 20),
                      analyst_base_before);
            EXPECT_EQ(world.ledger.entries(), ledger_before);
            EXPECT_EQ(world.reservations.find(1), reservation_before);
            EXPECT_FALSE(world.reservations.find(2).has_value());
            EXPECT_EQ(world.matching_engine.order_book().order_count(), 1U);
        }

        TEST(HackathonDemoSnapshotTest,
             NormalSnapshotIsDerivedFromTheExistingRuntimeScenario) {
            CapturingModelAdapter model;

            const HackathonDemoSnapshot snapshot = run_hackathon_demo_snapshot(
                HackathonDemoScenarioKind::Normal, &model);

            EXPECT_EQ(model.calls, 1U);
            EXPECT_EQ(snapshot.payment_preview_calls, 1U);
            EXPECT_TRUE(snapshot.turn.preview_authorized);
            EXPECT_EQ(snapshot.turn.preview.status,
                      ExternalPaymentPreviewStatus::Approved);
            EXPECT_EQ(snapshot.turn.risk_guidance.max_recommended_quantity, 1);
            EXPECT_EQ(snapshot.turn.action,
                      (AgentAction{SubmitOrderAction{Side::Buy, 100, 1}}));
            EXPECT_EQ(snapshot.turn.action_result,
                      (AgentActionResult{SubmitActionResult{
                          2, 2, SubmitResult::Accepted}}));
            ASSERT_TRUE(snapshot.execution.trade.has_value());
            EXPECT_EQ(snapshot.execution.trade->quantity, 1);
            EXPECT_TRUE(snapshot.turn.trader_objective.achieved);
            EXPECT_TRUE(snapshot.replay_available);
            EXPECT_EQ(snapshot.model_calls_replay, 0U);
            EXPECT_EQ(snapshot.payment_service_calls_replay, 0U);
            EXPECT_TRUE(snapshot.balance_parity);
            EXPECT_TRUE(snapshot.ledger_parity);
            EXPECT_TRUE(snapshot.objective_parity);
            EXPECT_TRUE(snapshot.order_parity);
            EXPECT_TRUE(snapshot.trade_parity);
        }

        TEST(HackathonDemoSnapshotTest,
             AgentErrorSnapshotShowsCoreSafetyWithoutCallingAModel) {
            const HackathonDemoSnapshot snapshot = run_hackathon_demo_snapshot(
                HackathonDemoScenarioKind::AgentError);

            EXPECT_EQ(snapshot.model_calls_original, 0U);
            EXPECT_EQ(snapshot.payment_preview_calls, 1U);
            EXPECT_EQ(snapshot.turn.risk_guidance.max_recommended_quantity, 1);
            EXPECT_EQ(snapshot.turn.action,
                      (AgentAction{SubmitOrderAction{Side::Buy, 100, 100}}));
            EXPECT_EQ(snapshot.turn.action_result,
                      (AgentActionResult{SubmitActionResult{
                          2, 2, SubmitResult::InsufficientFunds}}));
            EXPECT_FALSE(snapshot.turn.trader_objective.achieved);
            EXPECT_FALSE(snapshot.execution.trade.has_value());
            EXPECT_TRUE(snapshot.execution.balances_unchanged);
            EXPECT_TRUE(snapshot.execution.ledger_unchanged);
            EXPECT_TRUE(snapshot.execution.no_trader_reservation);
            EXPECT_TRUE(snapshot.execution.analyst_resting_order_unchanged);
            EXPECT_TRUE(snapshot.replay_available);
            EXPECT_EQ(snapshot.model_calls_replay, 0U);
            EXPECT_EQ(snapshot.payment_service_calls_replay, 0U);
            EXPECT_TRUE(snapshot.balance_parity);
            EXPECT_TRUE(snapshot.ledger_parity);
            EXPECT_TRUE(snapshot.objective_parity);
            EXPECT_TRUE(snapshot.order_parity);
            EXPECT_TRUE(snapshot.trade_parity);
        }

        TEST(HackathonSimulationTest,
             StartsRunsMultipleRoundsAndStopsWhenObjectiveIsAchieved) {
            SequencedModelAdapter model({
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
            });
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal,
                &model,
                HackathonSimulationConfig{3, 5});

            EXPECT_EQ(simulation.snapshot().status, HackathonSimulationStatus::Idle);
            simulation.start();
            EXPECT_EQ(simulation.snapshot().status, HackathonSimulationStatus::Running);
            EXPECT_EQ(simulation.snapshot().objective.target_amount, 3);

            simulation.advance();
            EXPECT_EQ(simulation.snapshot().rounds_completed, 1U);
            EXPECT_EQ(simulation.snapshot().objective.current_amount, 1);
            EXPECT_EQ(simulation.snapshot().match_engine.best_ask_before, 100);
            ASSERT_TRUE(simulation.snapshot().match_engine.trade.has_value());
            EXPECT_EQ(simulation.snapshot().match_engine.maker_remaining_quantity, 1);
            EXPECT_TRUE(simulation.snapshot().match_engine.reservation_consumed);
            EXPECT_EQ(simulation.snapshot().status, HackathonSimulationStatus::Running);

            simulation.advance();
            simulation.advance();
            const auto& finished = simulation.snapshot();
            EXPECT_EQ(finished.status, HackathonSimulationStatus::GoalAchieved);
            EXPECT_EQ(finished.rounds_completed, 3U);
            EXPECT_TRUE(finished.objective.achieved);
            ASSERT_TRUE(finished.summary.has_value());
            EXPECT_EQ(finished.summary->end_reason, "GOAL_ACHIEVED");
            EXPECT_EQ(finished.summary->deepseek_calls, 3U);
            EXPECT_EQ(finished.summary->analyst_service_accesses, 3U);
            EXPECT_EQ(finished.summary->accepted_actions, 3U);
            EXPECT_EQ(finished.summary->trades, 3U);
            EXPECT_EQ(finished.summary->score.objective_progress_points, 30);
            EXPECT_GT(finished.summary->score.total, 0);
            EXPECT_EQ(model.requests.size(), 3U);
            EXPECT_EQ(finished.activities.size(), 19U);
        }

        TEST(HackathonSimulationTest, StopIsHonoredAtTheNextRoundBoundary) {
            CapturingModelAdapter model;
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal,
                &model,
                HackathonSimulationConfig{5, 5});

            simulation.start();
            simulation.advance();
            const auto rounds_before_stop = simulation.snapshot().rounds_completed;
            simulation.request_stop();
            EXPECT_EQ(simulation.snapshot().status,
                      HackathonSimulationStatus::StopRequested);
            simulation.advance();

            const auto& stopped = simulation.snapshot();
            EXPECT_EQ(stopped.status, HackathonSimulationStatus::UserStopped);
            EXPECT_EQ(stopped.rounds_completed, rounds_before_stop);
            ASSERT_TRUE(stopped.summary.has_value());
            EXPECT_EQ(stopped.summary->end_reason, "USER_STOPPED");
            EXPECT_EQ(model.calls, 1U);
        }

        TEST(HackathonSimulationTest,
             AgentErrorPreservesFinancialStateAndReachesMaxRounds) {
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::AgentError,
                nullptr,
                HackathonSimulationConfig{5, 2});

            simulation.start();
            const Balance quote_before = simulation.snapshot().trader_quote;
            const std::size_t ledger_before = simulation.snapshot().ledger_entries;
            simulation.advance();
            ASSERT_TRUE(simulation.snapshot().last_turn.has_value());
            EXPECT_EQ(simulation.snapshot().last_turn->action,
                      (AgentAction{SubmitOrderAction{Side::Buy, 100, 100}}));
            EXPECT_EQ(simulation.snapshot().last_turn->action_result,
                      (AgentActionResult{SubmitActionResult{
                          6, 6, SubmitResult::InsufficientFunds}}));
            EXPECT_FALSE(simulation.snapshot().match_engine.trade.has_value());
            EXPECT_EQ(simulation.snapshot().trader_quote, quote_before);
            EXPECT_EQ(simulation.snapshot().ledger_entries, ledger_before);
            EXPECT_EQ(simulation.snapshot().active_orders, 5U);

            simulation.advance();
            const auto& finished = simulation.snapshot();
            EXPECT_EQ(finished.status, HackathonSimulationStatus::MaxRounds);
            ASSERT_TRUE(finished.summary.has_value());
            EXPECT_EQ(finished.summary->rejected_actions, 2U);
            EXPECT_EQ(finished.summary->trades, 0U);
            EXPECT_EQ(finished.summary->deepseek_calls, 0U);
        }

        TEST(HackathonSimulationTest,
             ManualReplayUsesCapturedEconomicInputsWithoutExternalCalls) {
            SequencedModelAdapter model({
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
            });
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal,
                &model,
                HackathonSimulationConfig{2, 3});
            simulation.start();
            simulation.advance();
            simulation.advance();
            ASSERT_EQ(simulation.snapshot().status,
                      HackathonSimulationStatus::GoalAchieved);
            EXPECT_EQ(simulation.snapshot().replay.status,
                      HackathonReplayStatus::NotRun);

            simulation.start_replay();
            ASSERT_EQ(simulation.snapshot().replay.status,
                      HackathonReplayStatus::Running);
            for (std::size_t step = 0;
                 simulation.snapshot().replay.status == HackathonReplayStatus::Running
                 && step < 12;
                 ++step) {
                simulation.advance_replay();
            }

            const auto& replay = simulation.snapshot().replay;
            EXPECT_EQ(replay.status, HackathonReplayStatus::Exact);
            EXPECT_EQ(replay.deepseek_calls_original, 2U);
            EXPECT_EQ(replay.deepseek_calls_replay, 0U);
            EXPECT_EQ(replay.payment_service_calls_replay, 0U);
            EXPECT_TRUE(replay.balance_parity);
            EXPECT_TRUE(replay.ledger_parity);
            EXPECT_TRUE(replay.objective_parity);
            EXPECT_TRUE(replay.order_parity);
            EXPECT_TRUE(replay.trade_parity);
            ASSERT_TRUE(replay.original_final_state.has_value());
            ASSERT_TRUE(replay.replay_final_state.has_value());
            EXPECT_EQ(*replay.original_final_state, *replay.replay_final_state);
            for (const auto& stage : replay.stages) {
                EXPECT_EQ(stage.state, "exact");
            }
        }

        TEST(HackathonSimulationTest,
             SameSeedProducesTheSamePublicScenarioAndRiskSequence) {
            SequencedModelAdapter first_model({
                R"({"action":"hold"})", R"({"action":"hold"})"});
            SequencedModelAdapter second_model({
                R"({"action":"hold"})", R"({"action":"hold"})"});
            const HackathonSimulationConfig config{5, 2, 0xBADC0FFEEULL};
            HackathonSimulation first(HackathonDemoScenarioKind::Normal, &first_model, config);
            HackathonSimulation second(HackathonDemoScenarioKind::Normal, &second_model, config);
            first.start(); second.start();
            first.advance(); second.advance();
            first.advance(); second.advance();

            ASSERT_EQ(first.snapshot().round_evidence.size(), 2U);
            EXPECT_EQ(first.snapshot().round_evidence, second.snapshot().round_evidence);
            EXPECT_FALSE(first.snapshot().round_evidence[0].analyst_public_reason.empty());
            EXPECT_EQ(first.snapshot().round_evidence[0].analyst_public_reason,
                      second.snapshot().round_evidence[0].analyst_public_reason);
            EXPECT_NE(first.snapshot().round_evidence[0].risk_guidance.risk_budget_bps,
                      first.snapshot().round_evidence[1].risk_guidance.risk_budget_bps);
            EXPECT_NE(first.snapshot().round_evidence[0].risk_guidance.local_best_ask, 0);
            EXPECT_NE(first.snapshot().round_evidence[0].external_market,
                      ExternalMarketSnapshot{});
        }

        TEST(HackathonSimulationTest,
             HoldIsAValidDecisionAndDoesNotMutateTheEconomicWorld) {
            SequencedModelAdapter model({R"({"action":"hold"})"});
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal, &model,
                HackathonSimulationConfig{5, 1, 0x5EED42});
            simulation.start();
            const Balance quote_before = simulation.snapshot().trader_quote;
            const std::size_t ledger_before = simulation.snapshot().ledger_entries;
            const std::size_t orders_before = simulation.snapshot().active_orders;
            simulation.advance();

            const auto& finished = simulation.snapshot();
            EXPECT_EQ(finished.status, HackathonSimulationStatus::MaxRounds);
            EXPECT_EQ(finished.trader_quote, quote_before);
            EXPECT_EQ(finished.ledger_entries, ledger_before);
            EXPECT_EQ(finished.active_orders, orders_before);
            ASSERT_EQ(finished.round_evidence.size(), 1U);
            EXPECT_TRUE(std::holds_alternative<HoldAction>(
                finished.round_evidence.front().action));
            EXPECT_TRUE(finished.round_evidence.front().trades.empty());
        }

        TEST(HackathonSimulationTest,
             ScoreChangesNeverBecomeAccountOrLedgerState) {
            SequencedModelAdapter weak_signal_model({R"({"action":"hold"})"});
            SequencedModelAdapter buy_signal_model({R"({"action":"hold"})"});
            HackathonSimulation weak_signal(
                HackathonDemoScenarioKind::Normal, &weak_signal_model,
                HackathonSimulationConfig{5, 1, 0x9876});
            HackathonSimulation buy_signal(
                HackathonDemoScenarioKind::Normal, &buy_signal_model,
                HackathonSimulationConfig{5, 1, 0x5EED42});
            weak_signal.start(); buy_signal.start();
            weak_signal.advance(); buy_signal.advance();

            EXPECT_NE(weak_signal.snapshot().score, buy_signal.snapshot().score);
            EXPECT_EQ(weak_signal.snapshot().trader_base,
                      buy_signal.snapshot().trader_base);
            EXPECT_EQ(weak_signal.snapshot().trader_quote,
                      buy_signal.snapshot().trader_quote);
            EXPECT_EQ(weak_signal.snapshot().ledger_entries,
                      buy_signal.snapshot().ledger_entries);
            EXPECT_EQ(weak_signal.snapshot().active_orders,
                      buy_signal.snapshot().active_orders);
        }

        TEST(HackathonSimulationTest,
             MultiLevelTakerEvidenceAndScoreStayOutsideEconomicCore) {
            SequencedModelAdapter model({
                R"({"action":"submit_order","side":"buy","price":108,"quantity":3})"});
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal, &model,
                HackathonSimulationConfig{3, 2, 0x1234});
            simulation.start();
            const std::size_t initial_ledger_entries = simulation.snapshot().ledger_entries;
            simulation.advance();

            const auto& finished = simulation.snapshot();
            EXPECT_EQ(finished.status, HackathonSimulationStatus::GoalAchieved);
            EXPECT_TRUE(finished.match_engine.multi_level_taker);
            EXPECT_EQ(finished.match_engine.maker_orders_consumed, 2U);
            ASSERT_EQ(finished.match_engine.trades.size(), 2U);
            EXPECT_EQ(finished.match_engine.trades[0].price, 100);
            EXPECT_EQ(finished.match_engine.trades[1].price, 101);
            EXPECT_EQ(finished.ledger_entries, initial_ledger_entries + 4U);
            EXPECT_EQ(finished.score.objective_progress_points, 30);
            EXPECT_LT(finished.score.risk_violation_points, 0);
            ASSERT_TRUE(finished.summary.has_value());
            EXPECT_EQ(finished.summary->trades, 2U);
            EXPECT_EQ(finished.summary->filled_base_quantity, 3);
            EXPECT_EQ(finished.summary->quote_spent, 301);
            EXPECT_EQ(finished.summary->current_reserved_quote, 0);
        }

        TEST(HackathonSimulationTest,
             PresentationEvidenceSeparatesExecutedQuoteFromRestingReservation) {
            SequencedModelAdapter model({
                R"({"action":"submit_order","side":"buy","price":108,"quantity":1})",
                R"({"action":"submit_order","side":"buy","price":100,"quantity":2})"});
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal, &model,
                HackathonSimulationConfig{5, 2, 0x1234});
            simulation.start();
            simulation.advance();
            simulation.advance();

            const auto& finished = simulation.snapshot();
            ASSERT_EQ(finished.status, HackathonSimulationStatus::MaxRounds);
            ASSERT_EQ(finished.round_evidence.size(), 2U);
            const auto& partial = finished.round_evidence[1];
            ASSERT_EQ(partial.trades.size(), 1U);
            EXPECT_EQ(partial.trades.front().quantity, 1);
            EXPECT_EQ(partial.trades.front().price, 100);
            ASSERT_TRUE(partial.resting_order.has_value());
            EXPECT_EQ(partial.resting_order->side, Side::Buy);
            EXPECT_EQ(partial.resting_order->price, 100);
            EXPECT_EQ(partial.resting_order->quantity, 1);
            EXPECT_EQ(partial.reserved_quote_after, 100);

            ASSERT_TRUE(finished.summary.has_value());
            EXPECT_EQ(finished.summary->trades, 2U);
            EXPECT_EQ(finished.summary->filled_base_quantity, 2);
            EXPECT_EQ(finished.summary->quote_spent, 200);
            EXPECT_EQ(finished.summary->current_reserved_quote, 100);
        }

        TEST(HackathonSimulationTest,
             ReplayCapturesAValidCancelActionWithoutExternalCalls) {
            SequencedModelAdapter model({
                R"({"action":"cancel_order","order_id":1})"});
            HackathonSimulation simulation(
                HackathonDemoScenarioKind::Normal, &model,
                HackathonSimulationConfig{5, 1, 0x5EED42});
            simulation.start();
            simulation.advance();
            ASSERT_EQ(simulation.snapshot().status, HackathonSimulationStatus::MaxRounds);
            ASSERT_TRUE(std::holds_alternative<CancelOrderAction>(
                simulation.snapshot().last_turn->action));

            simulation.start_replay();
            for (std::size_t step = 0;
                 simulation.snapshot().replay.status == HackathonReplayStatus::Running
                 && step < 8;
                 ++step) {
                simulation.advance_replay();
            }
            EXPECT_EQ(simulation.snapshot().replay.status, HackathonReplayStatus::Exact);
            EXPECT_EQ(simulation.snapshot().replay.deepseek_calls_replay, 0U);
            EXPECT_EQ(simulation.snapshot().replay.payment_service_calls_replay, 0U);
        }
    }  // namespace
}  // namespace exchange
