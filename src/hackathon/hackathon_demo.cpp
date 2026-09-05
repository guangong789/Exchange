#include "exchange/hackathon/hackathon_demo.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

#include "exchange/accounting/funding_coordinator.hpp"
#include "exchange/accounting/ledger.hpp"
#include "exchange/matching/event_collector.hpp"
#include "exchange/matching/matching_engine.hpp"
#include "exchange/model/model_adapter.hpp"

namespace exchange {
    namespace {
        void validate_requirement(const ExternalPaymentRequirement& requirement) {
            if (requirement.x402_version != 2 || requirement.resource_url.empty()
                || requirement.resource_description.empty()
                || requirement.resource_mime_type.empty()
                || requirement.scheme.empty() || requirement.network.empty()
                || requirement.amount.empty() || requirement.asset.empty()
                || requirement.pay_to.empty()
                || requirement.max_timeout_seconds == 0) {
                throw std::invalid_argument(
                    "premium service requires a complete x402 V2 requirement");
            }
        }

        void validate_signal(const PremiumMarketSignal& signal) {
            if (signal.signal.empty() || signal.confidence == 0
                || signal.confidence > 100 || signal.reason.empty()) {
                throw std::invalid_argument("premium market signal is invalid");
            }
        }

        void validate_config(
            const HackathonDemoConfig& config,
            const AgentRegistry& registry) {
            if (config.trader_agent_id == 0 || config.analyst_agent_id == 0
                || config.risk_agent_id == 0
                || config.trader_agent_id == config.analyst_agent_id
                || config.trader_agent_id == config.risk_agent_id
                || config.analyst_agent_id == config.risk_agent_id
                || config.trader_objective.asset_id == 0
                || config.trader_objective.target_amount <= 0) {
                throw std::invalid_argument("hackathon demo configuration is invalid");
            }
            if (!registry.find(config.trader_agent_id).has_value()
                || !registry.find(config.analyst_agent_id).has_value()
                || !registry.find(config.risk_agent_id).has_value()) {
                throw std::out_of_range(
                    "hackathon demo participant is not registered");
            }
        }

        constexpr AssetId demo_quote_asset = 10;
        constexpr AssetId demo_base_asset = 20;
        constexpr AccountId demo_trader_account = 1;
        constexpr AccountId demo_analyst_account = 2;
        constexpr AccountId demo_risk_account = 3;
        constexpr AccountId demo_treasury_account = 99;
        constexpr AgentId demo_trader_agent = 101;
        constexpr AgentId demo_analyst_agent = 202;
        constexpr AgentId demo_risk_agent = 303;
        constexpr InstrumentContext demo_instrument{
            demo_base_asset, demo_quote_asset, 1, 1, 1};
        constexpr AssetTargetObjective demo_objective{demo_base_asset, 1};

        struct DemoWorld {
            AccountStore accounts;
            OrderReservationStore reservations;
            EventCollector events;
            MatchingEngine matching_engine{events};
            Ledger ledger;
            ExecutionCoordinator execution{
                demo_instrument,
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
                demo_instrument};
            AgentActionGateway actions{registry, execution};
            FundingCoordinator funding{demo_treasury_account, accounts, ledger};
        };

        class CountingModelAdapter final : public ModelAdapter {
        public:
            explicit CountingModelAdapter(ModelAdapter& adapter) noexcept
                : adapter_(adapter) {}

            ModelResponse invoke(const ModelRequest& request) override {
                ++call_count_;
                return adapter_.invoke(request);
            }

            [[nodiscard]] std::size_t call_count() const noexcept {
                return call_count_;
            }

        private:
            ModelAdapter& adapter_;
            std::size_t call_count_{};
        };

        class UnusedModelAdapter final : public ModelAdapter {
        public:
            ModelResponse invoke(const ModelRequest&) override {
                throw std::logic_error("Agent Error scenario must not invoke a model");
            }
        };

        class OversizedBuyPolicy final : public PremiumSignalPolicy {
        public:
            [[nodiscard]] AgentAction decide(
                const AgentObservation&,
                const PremiumMarketSignal&,
                const RiskGuidance&) const override {
                return SubmitOrderAction{Side::Buy, 100, 100};
            }
        };

        [[nodiscard]] ExternalPaymentRequirement demo_payment_requirement() {
            return ExternalPaymentRequirement{
                2,
                "http://127.0.0.1/premium-signal",
                "Analyst Premium Market Signal",
                "application/json",
                "exact",
                "eip155:97",
                "10000",
                "0x330949Aed7d00FCe0558C64ED6FeC9792616cC39",
                "0x1111111111111111111111111111111111111111",
                60,
            };
        }

        [[nodiscard]] PremiumMarketSignal demo_premium_signal() {
            return PremiumMarketSignal{
                "BUY_BASE",
                95,
                "Local ask is within the deterministic entry threshold",
            };
        }

        [[nodiscard]] ExternalPaymentPreview demo_approved_preview(
            const ExternalPaymentRequirement& requirement) {
            return ExternalPaymentPreview{
                ExternalPaymentPreviewStatus::Approved,
                "deterministic-demo-preview",
                requirement.network,
                requirement.asset,
                requirement.amount,
                "READY_TO_SIGN",
                {},
            };
        }

        void initialize_demo_world(DemoWorld& world) {
            if (!world.accounts.create_account(demo_treasury_account)
                || !world.accounts.create_account(demo_trader_account)
                || !world.accounts.create_account(demo_analyst_account)
                || !world.accounts.create_account(demo_risk_account)
                || !world.registry.register_agent(
                    {demo_trader_agent, demo_trader_account})
                || !world.registry.register_agent(
                    {demo_analyst_agent, demo_analyst_account})
                || !world.registry.register_agent(
                    {demo_risk_agent, demo_risk_account})) {
                throw std::logic_error("failed to initialize hackathon demo world");
            }
            world.accounts.fund(demo_treasury_account, demo_quote_asset, 500);
            world.accounts.fund(demo_treasury_account, demo_base_asset, 2);
            if (world.funding.fund(demo_trader_account, demo_quote_asset, 500)
                    != FundingResult::Funded
                || world.funding.fund(demo_analyst_account, demo_base_asset, 2)
                    != FundingResult::Funded) {
                throw std::logic_error("failed to fund hackathon demo world");
            }
            const AgentActionResult resting = world.actions.execute(
                demo_analyst_agent, SubmitOrderAction{Side::Sell, 100, 1});
            const auto* submitted = std::get_if<SubmitActionResult>(&resting);
            if (submitted == nullptr || submitted->result != SubmitResult::Accepted) {
                throw std::logic_error("failed to rest analyst demo order");
            }
        }

        void initialize_simulation_world(
            DemoWorld& world,
            const HackathonSimulationConfig& config) {
            constexpr std::array<std::pair<Price, Quantity>, 5> sell_ladder{{
                {100, 2}, {101, 1}, {103, 3}, {106, 2}, {108, 4},
            }};
            constexpr Amount ladder_base = 12;
            constexpr Amount quote_buffer = 600;
            constexpr Price maximum_ladder_price = 108;
            if (config.target_base <= 0 || config.max_rounds == 0
                || config.target_base
                    > (std::numeric_limits<Amount>::max() - quote_buffer)
                        / maximum_ladder_price) {
                throw std::invalid_argument("simulation configuration is invalid");
            }
            if (!world.accounts.create_account(demo_treasury_account)
                || !world.accounts.create_account(demo_trader_account)
                || !world.accounts.create_account(demo_analyst_account)
                || !world.accounts.create_account(demo_risk_account)
                || !world.registry.register_agent(
                    {demo_trader_agent, demo_trader_account})
                || !world.registry.register_agent(
                    {demo_analyst_agent, demo_analyst_account})
                || !world.registry.register_agent(
                    {demo_risk_agent, demo_risk_account})) {
                throw std::logic_error("failed to initialize simulation world");
            }
            const Amount quote_funding = config.target_base * maximum_ladder_price
                + quote_buffer;
            world.accounts.fund(
                demo_treasury_account, demo_quote_asset, quote_funding);
            world.accounts.fund(
                demo_treasury_account, demo_base_asset, ladder_base);
            if (world.funding.fund(
                    demo_trader_account, demo_quote_asset, quote_funding)
                    != FundingResult::Funded
                || world.funding.fund(
                    demo_analyst_account, demo_base_asset, ladder_base)
                    != FundingResult::Funded) {
                throw std::logic_error("failed to fund simulation world");
            }
            for (const auto& [price, quantity] : sell_ladder) {
                const AgentActionResult resting = world.actions.execute(
                    demo_analyst_agent,
                    SubmitOrderAction{Side::Sell, price, quantity});
                const auto* submitted = std::get_if<SubmitActionResult>(&resting);
                if (submitted == nullptr
                    || submitted->result != SubmitResult::Accepted) {
                    throw std::logic_error("failed to rest simulation maker order");
                }
            }
        }

        [[nodiscard]] Balance balance_or_zero(
            const AccountStore& accounts,
            AccountId account_id,
            AssetId asset_id) {
            return accounts.find_balance(account_id, asset_id).value_or(Balance{});
        }

        [[nodiscard]] std::optional<Trade> latest_trade(const Ledger& ledger) {
            for (auto entry = ledger.entries().rbegin();
                 entry != ledger.entries().rend(); ++entry) {
                if (const auto* trade = std::get_if<TradeLedgerMetadata>(
                        &entry->transaction.metadata)) {
                    return trade->trade;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::vector<Trade> trades_since(
            const Ledger& ledger,
            std::size_t begin_index) {
            std::vector<Trade> result;
            const auto& entries = ledger.entries();
            for (std::size_t index = begin_index; index < entries.size(); ++index) {
                if (const auto* trade = std::get_if<TradeLedgerMetadata>(
                        &entries[index].transaction.metadata)) {
                    result.push_back(trade->trade);
                }
            }
            return result;
        }

        [[nodiscard]] std::size_t trade_count(const Ledger& ledger) {
            return static_cast<std::size_t>(std::count_if(
                ledger.entries().begin(), ledger.entries().end(),
                [](const LedgerEntry& entry) {
                    return std::holds_alternative<TradeLedgerMetadata>(
                        entry.transaction.metadata);
                }));
        }

        [[nodiscard]] Amount executed_quote_amount(const Ledger& ledger) {
            Amount total{};
            for (const LedgerEntry& entry : ledger.entries()) {
                const auto* trade = std::get_if<TradeLedgerMetadata>(
                    &entry.transaction.metadata);
                if (trade == nullptr) continue;
                const Amount amount = calculate_trade_amounts(
                    demo_instrument,
                    trade->trade.price,
                    trade->trade.quantity).quote_amount;
                if (amount > std::numeric_limits<Amount>::max() - total) {
                    throw std::overflow_error("executed quote total overflow");
                }
                total += amount;
            }
            return total;
        }

        [[nodiscard]] Amount filled_base_amount(const Ledger& ledger) {
            Amount total{};
            for (const LedgerEntry& entry : ledger.entries()) {
                const auto* trade = std::get_if<TradeLedgerMetadata>(
                    &entry.transaction.metadata);
                if (trade == nullptr) continue;
                const Amount amount = calculate_trade_amounts(
                    demo_instrument,
                    trade->trade.price,
                    trade->trade.quantity).base_amount;
                if (amount > std::numeric_limits<Amount>::max() - total) {
                    throw std::overflow_error("filled base total overflow");
                }
                total += amount;
            }
            return total;
        }

        [[nodiscard]] bool demo_balances_match(
            const DemoWorld& first,
            const DemoWorld& second) {
            for (const AccountId account : {
                     demo_trader_account, demo_analyst_account}) {
                for (const AssetId asset : {
                         demo_quote_asset, demo_base_asset}) {
                    if (first.accounts.find_balance(account, asset)
                        != second.accounts.find_balance(account, asset)) {
                        return false;
                    }
                }
            }
            return true;
        }
    }  // namespace

    DeterministicRiskAgent::DeterministicRiskAgent(
        std::uint16_t risk_budget_bps)
        : risk_budget_bps_(risk_budget_bps) {
        if (risk_budget_bps_ > 10'000) {
            throw std::invalid_argument("risk budget exceeds 10000 basis points");
        }
    }

    RiskGuidance DeterministicRiskAgent::assess(
        const AgentObservation& observation) const {
        const Amount available_quote = observation.quote_balance.has_value()
            ? std::max(observation.quote_balance->available, Amount{0})
            : Amount{0};
        const Price local_best_ask = observation.best_ask.value_or(Price{0});
        Amount objective_remaining{};
        if (observation.objective.has_value()) {
            const Amount target_amount = std::max(
                observation.objective->target_amount, Amount{0});
            const Amount current_amount = std::max(
                observation.objective->current_amount, Amount{0});
            if (target_amount > current_amount) {
                objective_remaining = target_amount - current_amount;
            }
        }

        Quantity affordable_quantity{};
        Quantity risk_budget_quantity{};
        if (local_best_ask > 0) {
            affordable_quantity = available_quote / local_best_ask;
            const Amount risk_budget_notional =
                (available_quote / 10'000) * risk_budget_bps_
                + (available_quote % 10'000) * risk_budget_bps_ / 10'000;
            risk_budget_quantity = risk_budget_notional / local_best_ask;
        }

        const Quantity max_recommended_quantity = std::min({
            affordable_quantity,
            risk_budget_quantity,
            static_cast<Quantity>(objective_remaining),
        });
        return RiskGuidance{
            available_quote,
            local_best_ask,
            risk_budget_bps_,
            affordable_quantity,
            risk_budget_quantity,
            objective_remaining,
            max_recommended_quantity,
            "取可负担数量、风险预算数量与目标剩余量的最小值",
        };
    }

    ModelPremiumSignalPolicy::ModelPremiumSignalPolicy(
        ModelAgentPolicy& model_policy) noexcept
        : model_policy_(model_policy) {}

    AgentAction ModelPremiumSignalPolicy::decide(
        const AgentObservation& observation,
        const PremiumMarketSignal& premium_signal,
        const RiskGuidance& risk_guidance) const {
        return decide_with_episode_context(
            observation, premium_signal, risk_guidance, {});
    }

    AgentAction PremiumSignalPolicy::decide_with_episode_context(
        const AgentObservation& observation,
        const PremiumMarketSignal& premium_signal,
        const RiskGuidance& risk_guidance,
        std::string_view) const {
        return decide(observation, premium_signal, risk_guidance);
    }

    AgentAction ModelPremiumSignalPolicy::decide_with_episode_context(
        const AgentObservation& observation,
        const PremiumMarketSignal& premium_signal,
        const RiskGuidance& risk_guidance,
        std::string_view episode_context) const {
        std::ostringstream context;
        context
            << "Analyst premium signal: " << premium_signal.signal << '\n'
            << "Analyst confidence: " << premium_signal.confidence << "%\n"
            << "Analyst reason: " << premium_signal.reason << '\n'
            << "Risk guidance:\n"
            << "  affordable_qty = "
            << risk_guidance.affordable_quantity << '\n'
            << "  risk_budget_qty = "
            << risk_guidance.risk_budget_quantity << '\n'
            << "  objective_remaining = "
            << risk_guidance.objective_remaining << '\n'
            << "  recommended_max_qty = "
            << risk_guidance.max_recommended_quantity << '\n'
            << "Risk reason: " << risk_guidance.reason << '\n'
            << "Risk guidance is advisory; you remain the final action owner. "
               "If submitting a buy, use the local best ask as the limit price "
               "and do not exceed the recommended quantity.";
        if (!episode_context.empty()) {
            context << '\n' << episode_context;
        }
        return model_policy_.decide(observation, context.str());
    }

    PremiumInformationService::PremiumInformationService(
        ExternalPaymentRequirement payment_requirement,
        PremiumMarketSignal signal)
        : payment_requirement_(std::move(payment_requirement)),
          signal_(std::move(signal)) {
        validate_requirement(payment_requirement_);
        validate_signal(signal_);
    }

    const ExternalPaymentRequirement&
    PremiumInformationService::payment_requirement() const noexcept {
        return payment_requirement_;
    }

    std::optional<PremiumMarketSignal>
    PremiumInformationService::access_after_preview(
        const ExternalPaymentPreview& preview) const {
        if (preview.status != ExternalPaymentPreviewStatus::Approved) {
            return std::nullopt;
        }
        return signal_;
    }

    HackathonEconomicReplayEvidence
    capture_hackathon_economic_replay_evidence(
        const AgentRegistry& registry,
        AgentId agent_id,
        const AgentAction& action,
        const AgentActionResult& result) {
        const auto identity = registry.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("replay evidence agent is not registered");
        }
        const auto* submit_action = std::get_if<SubmitOrderAction>(&action);
        const auto* submit_result = std::get_if<SubmitActionResult>(&result);
        if (submit_action == nullptr || submit_result == nullptr) {
            throw std::invalid_argument(
                "replay evidence requires a resolved submit action");
        }

        return HackathonEconomicReplayEvidence{
            identity->account_id,
            Order{
                submit_result->order_id,
                submit_action->side,
                OrderType::Limit,
                submit_action->price,
                submit_action->quantity,
                submit_result->timestamp},
            submit_result->result,
        };
    }

    SubmitResult replay_hackathon_economic_evidence(
        const HackathonEconomicReplayEvidence& evidence,
        ExecutionCoordinator& execution_coordinator) {
        return execution_coordinator.submit_order(
            OrderAdmissionRequest{evidence.account_id, evidence.order});
    }

    HackathonDemoSnapshot run_hackathon_demo_snapshot(
        HackathonDemoScenarioKind scenario_kind,
        ModelAdapter* model_adapter) {
        if (scenario_kind == HackathonDemoScenarioKind::Normal
            && model_adapter == nullptr) {
            throw std::invalid_argument("normal demo scenario requires a model adapter");
        }

        DemoWorld original;
        initialize_demo_world(original);
        const ExternalPaymentRequirement requirement = demo_payment_requirement();
        const Balance trader_base_before = balance_or_zero(
            original.accounts, demo_trader_account, demo_base_asset);
        const Balance trader_quote_before = balance_or_zero(
            original.accounts, demo_trader_account, demo_quote_asset);
        const Balance analyst_base_before = balance_or_zero(
            original.accounts, demo_analyst_account, demo_base_asset);
        const Balance analyst_quote_before = balance_or_zero(
            original.accounts, demo_analyst_account, demo_quote_asset);
        const std::vector<LedgerEntry> ledger_before = original.ledger.entries();

        DeterministicRiskAgent risk_agent(2'000);
        UnusedModelAdapter unused_model_adapter;
        ModelAdapter& selected_model_adapter = model_adapter != nullptr
            ? *model_adapter
            : static_cast<ModelAdapter&>(unused_model_adapter);
        CountingModelAdapter counted_model(selected_model_adapter);
        ModelAgentPolicy model_policy(counted_model);
        ModelPremiumSignalPolicy model_trader_policy(model_policy);
        OversizedBuyPolicy oversized_trader_policy;
        const PremiumSignalPolicy& trader_policy =
            scenario_kind == HackathonDemoScenarioKind::Normal
            ? static_cast<const PremiumSignalPolicy&>(model_trader_policy)
            : static_cast<const PremiumSignalPolicy&>(oversized_trader_policy);
        DeterministicRiskAgent const& advisory_risk_agent = risk_agent;
        HackathonDemoScenario scenario(
            HackathonDemoConfig{
                demo_trader_agent,
                demo_analyst_agent,
                demo_risk_agent,
                demo_objective},
            PremiumInformationService(requirement, demo_premium_signal()),
            original.registry,
            original.observations,
            original.actions,
            advisory_risk_agent,
            trader_policy);

        std::size_t payment_preview_calls{};
        const HackathonDemoTurn turn = scenario.run_once(
            [&payment_preview_calls](const ExternalPaymentRequirement& presented) {
                ++payment_preview_calls;
                return demo_approved_preview(presented);
            },
            ExternalMarketSnapshot{"BTCUSDT", 6'000'000, 6'000'100});

        HackathonDemoSnapshot snapshot;
        snapshot.scenario = scenario_kind;
        snapshot.payment_requirement = requirement;
        snapshot.turn = turn;
        snapshot.payment_preview_calls = payment_preview_calls;
        snapshot.model_calls_original = counted_model.call_count();
        snapshot.execution.trader_base_before = trader_base_before;
        snapshot.execution.trader_quote_before = trader_quote_before;
        snapshot.execution.analyst_base_before = analyst_base_before;
        snapshot.execution.analyst_quote_before = analyst_quote_before;
        snapshot.execution.trader_base_after = balance_or_zero(
            original.accounts, demo_trader_account, demo_base_asset);
        snapshot.execution.trader_quote_after = balance_or_zero(
            original.accounts, demo_trader_account, demo_quote_asset);
        snapshot.execution.analyst_base_after = balance_or_zero(
            original.accounts, demo_analyst_account, demo_base_asset);
        snapshot.execution.analyst_quote_after = balance_or_zero(
            original.accounts, demo_analyst_account, demo_quote_asset);
        snapshot.execution.trade = latest_trade(original.ledger);
        snapshot.execution.ledger_entry_count = original.ledger.entries().size();
        snapshot.execution.balances_unchanged =
            snapshot.execution.trader_base_before
                    == snapshot.execution.trader_base_after
                && snapshot.execution.trader_quote_before
                    == snapshot.execution.trader_quote_after
                && snapshot.execution.analyst_base_before
                    == snapshot.execution.analyst_base_after
                && snapshot.execution.analyst_quote_before
                    == snapshot.execution.analyst_quote_after;
        snapshot.execution.ledger_unchanged =
            original.ledger.entries() == ledger_before;
        snapshot.execution.no_trader_reservation =
            !original.reservations.find(2).has_value();
        snapshot.execution.analyst_resting_order_unchanged =
            original.matching_engine.order_book().order_count() == 1;

        const auto* submit_action = std::get_if<SubmitOrderAction>(&turn.action);
        const auto* submit_result = std::get_if<SubmitActionResult>(
            &turn.action_result);
        if (submit_action == nullptr || submit_result == nullptr) {
            return snapshot;
        }

        const HackathonEconomicReplayEvidence evidence =
            capture_hackathon_economic_replay_evidence(
                original.registry,
                demo_trader_agent,
                turn.action,
                turn.action_result);
        DemoWorld replay;
        initialize_demo_world(replay);
        const std::size_t payment_preview_calls_before_replay =
            payment_preview_calls;
        const std::size_t model_calls_before_replay = counted_model.call_count();
        const SubmitResult replay_result = replay_hackathon_economic_evidence(
            evidence, replay.execution);
        snapshot.model_calls_replay =
            counted_model.call_count() - model_calls_before_replay;
        snapshot.payment_service_calls_replay =
            payment_preview_calls - payment_preview_calls_before_replay;
        snapshot.replay_available = true;
        snapshot.balance_parity = demo_balances_match(original, replay);
        snapshot.ledger_parity = original.ledger.entries() == replay.ledger.entries();
        snapshot.objective_parity = turn.trader_objective == *replay.observations
            .observe(demo_trader_agent, demo_objective)
            .objective;
        snapshot.order_parity = original.matching_engine.order_book().order_count()
            == replay.matching_engine.order_book().order_count();
        snapshot.trade_parity = latest_trade(original.ledger)
            == latest_trade(replay.ledger);
        if (replay_result != evidence.expected_result
            || snapshot.model_calls_replay != 0
            || snapshot.payment_service_calls_replay != 0
            || !snapshot.balance_parity
            || !snapshot.ledger_parity
            || !snapshot.objective_parity
            || !snapshot.order_parity
            || !snapshot.trade_parity) {
            throw std::logic_error("fresh-world hackathon replay parity failed");
        }
        return snapshot;
    }

    class HackathonSimulation::Impl {
    public:
        Impl(
            HackathonDemoScenarioKind scenario,
            ModelAdapter* model_adapter,
            HackathonSimulationConfig config)
            : scenario_(scenario), config_(config) {
            if (config_.target_base <= 0 || config_.max_rounds == 0) {
                throw std::invalid_argument("simulation configuration is invalid");
            }
            if (scenario_ == HackathonDemoScenarioKind::Normal
                && model_adapter == nullptr) {
                throw std::invalid_argument(
                    "normal simulation requires a model adapter");
            }
            ModelAdapter& selected = model_adapter != nullptr
                ? *model_adapter
                : static_cast<ModelAdapter&>(unused_model_);
            counted_model_ = std::make_unique<CountingModelAdapter>(selected);
            model_policy_ = std::make_unique<ModelAgentPolicy>(*counted_model_);
            model_trader_policy_ = std::make_unique<ModelPremiumSignalPolicy>(
                *model_policy_);
            snapshot_.scenario = scenario_;
            snapshot_.config = config_;
            snapshot_.replay.stages = {
                {"Rebuild fresh world", "pending"},
                {"Restore deterministic genesis", "pending"},
                {"Replay captured orders/actions", "pending"},
                {"Compare balances, Ledger, orders and trades", "pending"},
                {"Recompute objective", "pending"},
            };
        }

        void start() {
            if (snapshot_.status != HackathonSimulationStatus::Idle) {
                throw std::logic_error("simulation may only start from IDLE");
            }
            initialize_simulation_world(original_, config_);
            initial_trader_base_ = balance_or_zero(
                original_.accounts, demo_trader_account, demo_base_asset);
            initial_trader_quote_ = balance_or_zero(
                original_.accounts, demo_trader_account, demo_quote_asset);
            snapshot_.status = HackathonSimulationStatus::Running;
            refresh_live_snapshot();
            append_activity("System", "simulation started; seed="
                + std::to_string(config_.seed)
                + "; account-backed sell ladder=2@100,1@101,3@103,2@106,4@108");
        }

        void advance() {
            if (snapshot_.status == HackathonSimulationStatus::StopRequested) {
                snapshot_.status = HackathonSimulationStatus::UserStopped;
                finish_episode();
                return;
            }
            if (snapshot_.status != HackathonSimulationStatus::Running) {
                throw std::logic_error("simulation is not running");
            }

            try {
                run_round();
            } catch (const std::exception& error) {
                snapshot_.status = HackathonSimulationStatus::Error;
                append_activity("System", std::string("runtime error: ") + error.what());
                refresh_live_snapshot();
                finish_episode();
            }
        }

        void request_stop() {
            if (snapshot_.status == HackathonSimulationStatus::Running) {
                snapshot_.status = HackathonSimulationStatus::StopRequested;
                append_activity("System", "stop requested at next safe round boundary");
            }
        }

        void start_replay() {
            if (!is_terminal(snapshot_.status)) {
                throw std::logic_error("replay requires a completed simulation");
            }
            if (snapshot_.replay.status != HackathonReplayStatus::NotRun) {
                throw std::logic_error("replay has already been started");
            }
            snapshot_.replay.status = HackathonReplayStatus::Running;
        }

        void advance_replay() {
            if (snapshot_.replay.status != HackathonReplayStatus::Running) {
                throw std::logic_error("replay is not running");
            }
            switch (replay_phase_) {
                case 0:
                    replay_ = std::make_unique<DemoWorld>();
                    complete_replay_stage(0);
                    ++replay_phase_;
                    break;
                case 1:
                    initialize_simulation_world(*replay_, config_);
                    complete_replay_stage(1);
                    ++replay_phase_;
                    break;
                case 2:
                    replay_one_economic_input();
                    break;
                case 3:
                    compare_replay_economic_state();
                    complete_replay_stage(3);
                    ++replay_phase_;
                    break;
                case 4:
                    compare_replay_objective();
                    complete_replay_stage(4);
                    snapshot_.replay.status = replay_is_exact()
                        ? HackathonReplayStatus::Exact
                        : HackathonReplayStatus::Mismatch;
                    break;
                default:
                    throw std::logic_error("replay phase is invalid");
            }
        }

        [[nodiscard]] const HackathonSimulationSnapshot& snapshot() const noexcept {
            return snapshot_;
        }

    private:
        [[nodiscard]] static bool is_terminal(HackathonSimulationStatus status) {
            return status == HackathonSimulationStatus::GoalAchieved
                || status == HackathonSimulationStatus::UserStopped
                || status == HackathonSimulationStatus::MaxRounds
                || status == HackathonSimulationStatus::Error;
        }

        [[nodiscard]] static std::string submit_result_name(SubmitResult result) {
            switch (result) {
                case SubmitResult::Accepted: return "Accepted";
                case SubmitResult::InsufficientFunds: return "InsufficientFunds";
                case SubmitResult::DuplicateOrder: return "DuplicateOrder";
                case SubmitResult::InvalidOrder: return "InvalidOrder";
                case SubmitResult::CounterpartyNotAccountBacked:
                    return "CounterpartyNotAccountBacked";
            }
            return "Unknown";
        }

        [[nodiscard]] static std::string action_name(const AgentAction& action) {
            return std::visit(
                [](const auto& value) -> std::string {
                    using Action = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
                        return std::string(value.side == Side::Buy ? "BUY " : "SELL ")
                            + std::to_string(value.quantity) + " @ "
                            + std::to_string(value.price);
                    } else if constexpr (std::is_same_v<Action, CancelOrderAction>) {
                        return "CANCEL " + std::to_string(value.order_id);
                    }
                    return "HOLD";
                },
                action);
        }

        struct RoundScenarioContext {
            ExternalMarketSnapshot external_market;
            PremiumMarketSignal analyst_signal;
            std::string analyst_public_reason;
            std::uint16_t risk_budget_bps{};
        };

        [[nodiscard]] static std::uint64_t mix_seed(std::uint64_t value) noexcept {
            value += 0x9E3779B97F4A7C15ULL;
            value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        }

        [[nodiscard]] RoundScenarioContext scenario_context_for_next_round() const {
            const std::size_t round = snapshot_.rounds_completed + 1;
            const std::uint64_t value = mix_seed(
                config_.seed ^ static_cast<std::uint64_t>(round));
            const Price local_ask = original_.matching_engine.order_book()
                .best_ask().value_or(Price{108});
            const Price movement = static_cast<Price>(value % 9U) - 4;
            const Price external_bid = std::max(Price{1}, local_ask + movement);
            const Price external_ask = external_bid + 1;
            const bool hold_signal = value % 5U == 0U;
            const std::uint32_t confidence = hold_signal
                ? 55U + static_cast<std::uint32_t>((value >> 8U) % 12U)
                : 70U + static_cast<std::uint32_t>((value >> 8U) % 26U);
            const std::uint16_t risk_budget_bps = std::array<std::uint16_t, 4>{
                1'000, 1'800, 2'500, 3'000,
            }[(value >> 16U) % 4U];
            const std::string signal = hold_signal ? "HOLD" : "BUY_BASE";
            const std::string public_reason = hold_signal
                ? "当前 seeded regime 标记为暂不交易，本轮保持 HOLD。"
                : external_bid > local_ask
                ? "外部参考高于本地最优卖价，当前 seeded regime 支持积累。"
                : external_ask < local_ask
                ? "本地最优卖价高于外部参考，当前 seeded regime 仍支持积累。"
                : "本地价格接近外部参考，当前 seeded regime 支持积累。";
            return RoundScenarioContext{
                ExternalMarketSnapshot{
                    "BASEQUOTE", external_bid, external_ask},
                PremiumMarketSignal{
                    signal,
                    confidence,
                    "seeded market context: local ask="
                        + std::to_string(local_ask)
                        + ", external reference="
                        + std::to_string(external_bid)
                        + "/" + std::to_string(external_ask),
                },
                public_reason,
                risk_budget_bps,
            };
        }

        [[nodiscard]] std::string trader_episode_context() const {
            std::ostringstream context;
            context << "Episode context:\n"
                    << "  round = " << snapshot_.rounds_completed + 1 << '\n'
                    << "  previous outcome = " << previous_round_outcome_ << '\n'
                    << "  episode score = " << snapshot_.score.total << '\n';
            return context.str();
        }

        void apply_round_score(
            const HackathonDemoTurn& turn,
            Amount objective_before,
            const ObjectiveProgress& objective_after,
            HackathonRoundEvidence& evidence) {
            evidence.score_before = snapshot_.score.total;
            const Amount gained = std::max(
                objective_after.current_amount - objective_before, Amount{0});
            if (gained > 0) {
                const std::int64_t points = static_cast<std::int64_t>(gained) * 10;
                snapshot_.score.objective_progress_points += points;
                evidence.score_delta += points;
                evidence.score_reasons.push_back(
                    "+" + std::to_string(points) + " 目标进度");
            }
            if (const auto* submitted = std::get_if<SubmitActionResult>(
                    &turn.action_result)) {
                if (submitted->result == SubmitResult::Accepted) {
                    snapshot_.score.accepted_action_points += 2;
                    evidence.score_delta += 2;
                    evidence.score_reasons.push_back("+2 有效经济执行");
                } else {
                    snapshot_.score.rejected_action_points -= 2;
                    evidence.score_delta -= 2;
                    evidence.score_reasons.push_back("-2 经济行为被拒绝");
                }
            }
            if (const auto* submit = std::get_if<SubmitOrderAction>(&turn.action)) {
                if (submit->quantity <= turn.risk_guidance.max_recommended_quantity) {
                    ++snapshot_.score.risk_compliant_points;
                    ++evidence.score_delta;
                    evidence.score_reasons.push_back("+1 符合 Risk 建议");
                } else {
                    snapshot_.score.risk_violation_points -= 3;
                    evidence.score_delta -= 3;
                    evidence.score_reasons.push_back("-3 明显超出 Risk 建议");
                }
            } else if (std::holds_alternative<HoldAction>(turn.action)
                       && turn.premium_signal.has_value()
                       && (turn.premium_signal->signal == "HOLD"
                           || turn.premium_signal->confidence < 65U)) {
                ++snapshot_.score.useful_hold_points;
                ++evidence.score_delta;
                evidence.score_reasons.push_back("+1 有效 HOLD");
            }
            snapshot_.score.total += evidence.score_delta;
            evidence.score_after = snapshot_.score.total;
        }

        void append_activity(std::string role, std::string detail) {
            snapshot_.activities.push_back(HackathonSimulationActivity{
                snapshot_.activities.size() + 1,
                snapshot_.rounds_completed,
                std::move(role),
                std::move(detail),
            });
        }

        void refresh_live_snapshot() {
            snapshot_.trader_base = balance_or_zero(
                original_.accounts, demo_trader_account, demo_base_asset);
            snapshot_.trader_quote = balance_or_zero(
                original_.accounts, demo_trader_account, demo_quote_asset);
            snapshot_.analyst_base = balance_or_zero(
                original_.accounts, demo_analyst_account, demo_base_asset);
            snapshot_.analyst_quote = balance_or_zero(
                original_.accounts, demo_analyst_account, demo_quote_asset);
            snapshot_.ledger_entries = original_.ledger.entries().size();
            snapshot_.active_orders = original_.matching_engine.order_book().order_count();
            snapshot_.trade_count = trade_count(original_.ledger);
            snapshot_.objective = *original_.observations.observe(
                demo_trader_agent,
                AssetTargetObjective{demo_base_asset, config_.target_base})
                .objective;
            snapshot_.match_engine.active_order_count = snapshot_.active_orders;
        }

        void run_round() {
            const std::size_t ledger_before_count = original_.ledger.entries().size();
            const Amount objective_before = snapshot_.objective.current_amount;
            const Balance trader_base_before = balance_or_zero(
                original_.accounts, demo_trader_account, demo_base_asset);
            const Balance trader_quote_before = balance_or_zero(
                original_.accounts, demo_trader_account, demo_quote_asset);
            const Balance analyst_base_before = balance_or_zero(
                original_.accounts, demo_analyst_account, demo_base_asset);
            const Balance analyst_quote_before = balance_or_zero(
                original_.accounts, demo_analyst_account, demo_quote_asset);
            const std::vector<LedgerEntry> ledger_before = original_.ledger.entries();
            const auto best_bid_before = original_.matching_engine.order_book().best_bid();
            const auto best_ask_before = original_.matching_engine.order_book().best_ask();
            const RoundScenarioContext scenario_context =
                scenario_context_for_next_round();
            const DeterministicRiskAgent round_risk_agent(
                scenario_context.risk_budget_bps);

            const PremiumSignalPolicy& trader_policy =
                scenario_ == HackathonDemoScenarioKind::Normal
                ? static_cast<const PremiumSignalPolicy&>(*model_trader_policy_)
                : static_cast<const PremiumSignalPolicy&>(oversized_policy_);
            HackathonDemoScenario round_scenario(
                HackathonDemoConfig{
                    demo_trader_agent,
                    demo_analyst_agent,
                    demo_risk_agent,
                    AssetTargetObjective{demo_base_asset, config_.target_base}},
                PremiumInformationService(
                    demo_payment_requirement(), scenario_context.analyst_signal),
                original_.registry,
                original_.observations,
                original_.actions,
                round_risk_agent,
                trader_policy);
            const HackathonDemoTurn turn = round_scenario.run_once(
                [this](const ExternalPaymentRequirement& requirement) {
                    ++payment_service_accesses_;
                    return demo_approved_preview(requirement);
                },
                scenario_context.external_market,
                trader_episode_context());

            ++snapshot_.rounds_completed;
            ++trader_decisions_;
            snapshot_.last_turn = turn;
            snapshot_.match_engine = {};
            snapshot_.match_engine.best_bid_before = best_bid_before;
            snapshot_.match_engine.best_ask_before = best_ask_before;
            snapshot_.match_engine.price_time_priority = true;

            if (const auto* submitted = std::get_if<SubmitActionResult>(
                    &turn.action_result)) {
                ++orders_submitted_;
                if (submitted->result == SubmitResult::Accepted) {
                    ++accepted_actions_;
                } else {
                    ++snapshot_.rejected_actions;
                }
                if (const auto* action = std::get_if<SubmitOrderAction>(&turn.action)) {
                    snapshot_.match_engine.incoming_order = Order{
                        submitted->order_id,
                        action->side,
                        OrderType::Limit,
                        action->price,
                        action->quantity,
                        submitted->timestamp};
                    snapshot_.match_engine.reservation_consumed =
                        submitted->result == SubmitResult::Accepted
                        && !original_.reservations.find(submitted->order_id).has_value();
                }
            } else if (std::holds_alternative<HoldActionResult>(turn.action_result)) {
                ++held_actions_;
            }
            if (!std::holds_alternative<HoldAction>(turn.action)) {
                economic_actions_.push_back(
                    CapturedGatewayAction{turn.action, turn.action_result});
                snapshot_.replay.captured_economic_inputs.push_back(
                    action_name(turn.action));
            }

            snapshot_.match_engine.trades = trades_since(
                original_.ledger, ledger_before_count);
            if (!snapshot_.match_engine.trades.empty()) {
                snapshot_.match_engine.trade = snapshot_.match_engine.trades.back();
            }
            if (snapshot_.match_engine.trade.has_value()) {
                const auto maker = original_.matching_engine.order_book().find_order(
                    snapshot_.match_engine.trade->sell_order_id);
                snapshot_.match_engine.maker_remaining_quantity = maker.has_value()
                    ? maker->quantity : Quantity{0};
            }
            std::set<OrderId> maker_orders;
            for (const Trade& trade : snapshot_.match_engine.trades) {
                maker_orders.insert(trade.sell_order_id);
                if (const auto maker = original_.matching_engine.order_book().find_order(
                        trade.sell_order_id);
                    maker.has_value() && maker->quantity > 0) {
                    snapshot_.match_engine.partial_fill = true;
                }
            }
            snapshot_.match_engine.maker_orders_consumed = maker_orders.size();
            snapshot_.match_engine.multi_level_taker = maker_orders.size() > 1;
            refresh_live_snapshot();
            std::optional<Order> resting_order;
            if (const auto* submitted = std::get_if<SubmitActionResult>(
                    &turn.action_result);
                submitted != nullptr && submitted->result == SubmitResult::Accepted) {
                resting_order = original_.matching_engine.order_book().find_order(
                    submitted->order_id);
            }
            const bool balances_unchanged =
                trader_base_before == snapshot_.trader_base
                && trader_quote_before == snapshot_.trader_quote
                && analyst_base_before == snapshot_.analyst_base
                && analyst_quote_before == snapshot_.analyst_quote;
            const bool ledger_unchanged = original_.ledger.entries() == ledger_before;
            snapshot_.match_engine.balances_unchanged = balances_unchanged;
            snapshot_.match_engine.ledger_unchanged = ledger_unchanged;
            if (const auto* submitted = std::get_if<SubmitActionResult>(
                    &turn.action_result)) {
                snapshot_.match_engine.no_residual_reservation =
                    !original_.reservations.find(submitted->order_id).has_value();
                if (submitted->result != SubmitResult::Accepted
                    && (!balances_unchanged || !ledger_unchanged
                        || !snapshot_.match_engine.no_residual_reservation)) {
                    ++invalid_state_mutations_;
                }
            }

            HackathonRoundEvidence round_evidence{
                snapshot_.rounds_completed,
                scenario_context.external_market,
                scenario_context.analyst_signal,
                scenario_context.analyst_public_reason,
                turn.risk_guidance,
                turn.action,
                turn.action_result,
                snapshot_.match_engine.trades,
                resting_order,
                snapshot_.trader_quote.reserved,
                snapshot_.objective,
                0,
                0,
                0,
                {},
            };
            apply_round_score(
                turn, objective_before, snapshot_.objective, round_evidence);
            snapshot_.round_evidence.push_back(round_evidence);

            append_activity("Analyst", "PreviewAuthorized; signal="
                + turn.premium_signal.value_or(PremiumMarketSignal{}).signal
                + "; confidence=" + std::to_string(
                    turn.premium_signal.value_or(PremiumMarketSignal{}).confidence)
                + "; reason="
                + turn.premium_signal.value_or(PremiumMarketSignal{}).reason);
            append_activity("Risk", "affordable="
                + std::to_string(turn.risk_guidance.affordable_quantity)
                + "; risk_budget="
                + std::to_string(turn.risk_guidance.risk_budget_quantity)
                + "; objective_remaining="
                + std::to_string(turn.risk_guidance.objective_remaining)
                + "; recommended="
                + std::to_string(turn.risk_guidance.max_recommended_quantity));
            append_activity("Trader", action_name(turn.action));
            if (const auto* submitted = std::get_if<SubmitActionResult>(
                    &turn.action_result)) {
                std::string detail = submit_result_name(submitted->result);
                if (snapshot_.match_engine.trade.has_value()) {
                    detail += "; Trade "
                        + std::to_string(snapshot_.match_engine.trade->quantity)
                        + " @ " + std::to_string(snapshot_.match_engine.trade->price);
                } else if (submitted->result != SubmitResult::Accepted) {
                    detail += balances_unchanged && ledger_unchanged
                        && !original_.reservations.find(submitted->order_id).has_value()
                        ? "; balances and Ledger unchanged"
                        : "; inspect runtime evidence";
                }
                append_activity("Core", std::move(detail));
            } else {
                append_activity("Core", "HoldAction completed without economic mutation");
            }
            append_activity("Score", "before="
                + std::to_string(round_evidence.score_before)
                + "; delta=" + std::to_string(round_evidence.score_delta)
                + "; after=" + std::to_string(round_evidence.score_after));
            append_activity("Objective", std::to_string(snapshot_.objective.current_amount)
                + " / " + std::to_string(snapshot_.objective.target_amount)
                + " BASE");
            previous_round_outcome_ = action_name(turn.action) + " -> "
                + std::visit(
                    [](const auto& result) -> std::string {
                        using Result = std::decay_t<decltype(result)>;
                        if constexpr (std::is_same_v<Result, SubmitActionResult>) {
                            return submit_result_name(result.result);
                        } else if constexpr (std::is_same_v<Result, CancelActionResult>) {
                            return result.result == CancelResult::Cancelled
                                ? "Cancelled" : "CancelRejected";
                        }
                        return "Held";
                    },
                    turn.action_result);

            if (snapshot_.objective.achieved) {
                snapshot_.status = HackathonSimulationStatus::GoalAchieved;
                finish_episode();
            } else if (snapshot_.rounds_completed >= config_.max_rounds) {
                snapshot_.status = HackathonSimulationStatus::MaxRounds;
                finish_episode();
            }
        }

        void finish_episode() {
            refresh_live_snapshot();
            snapshot_.summary = HackathonSimulationSummary{
                end_reason(snapshot_.status),
                config_.seed,
                snapshot_.rounds_completed,
                counted_model_->call_count(),
                payment_service_accesses_,
                trader_decisions_,
                orders_submitted_,
                accepted_actions_,
                snapshot_.rejected_actions,
                held_actions_,
                snapshot_.trade_count,
                std::max(snapshot_.trader_base.available - initial_trader_base_.available,
                         Amount{0}),
                executed_quote_amount(original_.ledger),
                snapshot_.trader_quote.reserved,
                filled_base_amount(original_.ledger),
                snapshot_.objective.current_amount,
                snapshot_.objective.target_amount,
                snapshot_.ledger_entries,
                snapshot_.active_orders,
                invalid_state_mutations_,
                snapshot_.score,
            };
        }

        [[nodiscard]] static std::string end_reason(
            HackathonSimulationStatus status) {
            switch (status) {
                case HackathonSimulationStatus::GoalAchieved: return "GOAL_ACHIEVED";
                case HackathonSimulationStatus::UserStopped: return "USER_STOPPED";
                case HackathonSimulationStatus::MaxRounds: return "MAX_ROUNDS";
                case HackathonSimulationStatus::Error: return "ERROR";
                default: return "INCOMPLETE";
            }
        }

        void complete_replay_stage(std::size_t index) {
            snapshot_.replay.stages[index].state = "exact";
        }

        void replay_one_economic_input() {
            snapshot_.replay.stages[2].state = "running";
            if (replay_input_index_ < economic_actions_.size()) {
                const CapturedGatewayAction& evidence =
                    economic_actions_[replay_input_index_++];
                const AgentActionResult result = replay_->actions.execute(
                    demo_trader_agent, evidence.action);
                if (result != evidence.expected_result) {
                    snapshot_.replay.stages[2].state = "mismatch";
                    snapshot_.replay.status = HackathonReplayStatus::Mismatch;
                    return;
                }
                return;
            }
            complete_replay_stage(2);
            ++replay_phase_;
        }

        void compare_replay_economic_state() {
            snapshot_.replay.stages[3].state = "running";
            snapshot_.replay.deepseek_calls_original = counted_model_->call_count();
            snapshot_.replay.deepseek_calls_replay = 0;
            snapshot_.replay.payment_service_calls_original = payment_service_accesses_;
            snapshot_.replay.payment_service_calls_replay = 0;
            snapshot_.replay.original_final_state = replay_world_state(original_);
            snapshot_.replay.replay_final_state = replay_world_state(*replay_);
            snapshot_.replay.balance_parity = demo_balances_match(original_, *replay_);
            snapshot_.replay.ledger_parity = original_.ledger.entries()
                == replay_->ledger.entries();
            snapshot_.replay.order_parity = original_.matching_engine.order_book().order_count()
                == replay_->matching_engine.order_book().order_count();
            snapshot_.replay.trade_parity = latest_trade(original_.ledger)
                == latest_trade(replay_->ledger);
        }

        [[nodiscard]] HackathonReplayWorldState replay_world_state(
            const DemoWorld& world) const {
            const AgentObservation observation = world.observations.observe(
                demo_trader_agent,
                AssetTargetObjective{demo_base_asset, config_.target_base});
            if (!observation.objective.has_value()) {
                throw std::logic_error("replay world is missing objective state");
            }
            return HackathonReplayWorldState{
                balance_or_zero(world.accounts, demo_trader_account, demo_base_asset),
                balance_or_zero(world.accounts, demo_trader_account, demo_quote_asset),
                trade_count(world.ledger),
                filled_base_amount(world.ledger),
                executed_quote_amount(world.ledger),
                world.matching_engine.order_book().order_count(),
                world.ledger.entries().size(),
                *observation.objective,
            };
        }

        void compare_replay_objective() {
            snapshot_.replay.stages[4].state = "running";
            const ObjectiveProgress replay_objective = *replay_->observations.observe(
                demo_trader_agent,
                AssetTargetObjective{demo_base_asset, config_.target_base})
                .objective;
            snapshot_.replay.objective_parity = snapshot_.objective == replay_objective;
        }

        [[nodiscard]] bool replay_is_exact() const {
            return snapshot_.replay.deepseek_calls_replay == 0
                && snapshot_.replay.payment_service_calls_replay == 0
                && snapshot_.replay.balance_parity
                && snapshot_.replay.ledger_parity
                && snapshot_.replay.objective_parity
                && snapshot_.replay.order_parity
                && snapshot_.replay.trade_parity;
        }

        struct CapturedGatewayAction {
            AgentAction action;
            AgentActionResult expected_result;
        };

        HackathonDemoScenarioKind scenario_;
        HackathonSimulationConfig config_;
        DemoWorld original_;
        UnusedModelAdapter unused_model_;
        std::unique_ptr<CountingModelAdapter> counted_model_;
        std::unique_ptr<ModelAgentPolicy> model_policy_;
        std::unique_ptr<ModelPremiumSignalPolicy> model_trader_policy_;
        OversizedBuyPolicy oversized_policy_;
        HackathonSimulationSnapshot snapshot_;
        Balance initial_trader_base_;
        Balance initial_trader_quote_;
        std::size_t payment_service_accesses_{};
        std::size_t trader_decisions_{};
        std::size_t orders_submitted_{};
        std::size_t accepted_actions_{};
        std::size_t held_actions_{};
        std::size_t invalid_state_mutations_{};
        std::string previous_round_outcome_{"no previous round"};
        std::vector<CapturedGatewayAction> economic_actions_;
        std::unique_ptr<DemoWorld> replay_;
        std::size_t replay_phase_{};
        std::size_t replay_input_index_{};
    };

    HackathonSimulation::HackathonSimulation(
        HackathonDemoScenarioKind scenario,
        ModelAdapter* model_adapter,
        HackathonSimulationConfig config)
        : impl_(std::make_unique<Impl>(scenario, model_adapter, config)) {}

    HackathonSimulation::~HackathonSimulation() = default;

    void HackathonSimulation::start() { impl_->start(); }
    void HackathonSimulation::advance() { impl_->advance(); }
    void HackathonSimulation::request_stop() { impl_->request_stop(); }
    void HackathonSimulation::start_replay() { impl_->start_replay(); }
    void HackathonSimulation::advance_replay() { impl_->advance_replay(); }
    const HackathonSimulationSnapshot& HackathonSimulation::snapshot() const noexcept {
        return impl_->snapshot();
    }

    HackathonDemoScenario::HackathonDemoScenario(
        HackathonDemoConfig config,
        PremiumInformationService premium_service,
        const AgentRegistry& registry,
        const AgentObservationService& observations,
        AgentActionGateway& actions,
        const DeterministicRiskAgent& risk_agent,
        const PremiumSignalPolicy& trader_policy)
        : config_(std::move(config)),
          premium_service_(std::move(premium_service)),
          observations_(observations),
          actions_(actions),
          risk_agent_(risk_agent),
          trader_policy_(trader_policy) {
        validate_config(config_, registry);
    }

    HackathonDemoTurn HackathonDemoScenario::run_once(
        const PaymentPreviewFunction& preview_payment,
        std::optional<ExternalMarketSnapshot> external_market,
        std::string_view episode_context) {
        if (run_called_) {
            throw std::logic_error("hackathon demo scenario may run only once");
        }
        if (!preview_payment) {
            throw std::invalid_argument("payment preview function is required");
        }
        run_called_ = true;

        ExternalPaymentPreview preview = preview_payment(
            premium_service_.payment_requirement());
        std::optional<PremiumMarketSignal> premium_signal =
            premium_service_.access_after_preview(preview);
        AgentObservation observation = observations_.observe(
            config_.trader_agent_id, config_.trader_objective);
        observation.external_market = std::move(external_market);
        const RiskGuidance risk_guidance = risk_agent_.assess(observation);
        const AgentAction action = premium_signal.has_value()
            ? trader_policy_.decide_with_episode_context(
                  observation, *premium_signal, risk_guidance, episode_context)
            : AgentAction{HoldAction{}};
        AgentActionResult action_result = actions_.execute(
            config_.trader_agent_id, action);
        const AgentObservation after = observations_.observe(
            config_.trader_agent_id, config_.trader_objective);

        return HackathonDemoTurn{
            std::move(preview),
            premium_signal.has_value(),
            std::move(premium_signal),
            risk_guidance,
            observation,
            action,
            std::move(action_result),
            *after.objective,
        };
    }
}  // namespace exchange
