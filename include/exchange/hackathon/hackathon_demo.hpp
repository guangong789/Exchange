#pragma once

#include "exchange/accounting/account.hpp"
#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/agent/external_market.hpp"
#include "exchange/core/types.hpp"
#include "exchange/matching/order.hpp"
#include "exchange/matching/trade.hpp"
#include <cstdint>
#include <cstddef>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "exchange/agent/agent.hpp"
#include "exchange/agent/agent_action_gateway.hpp"
#include "exchange/agent/agent_observation_service.hpp"
#include "exchange/agent/agent_registry.hpp"
#include "exchange/x402/external_payment_preview.hpp"
#include "exchange/model/model_agent_policy.hpp"
#include "exchange/x402/x402.hpp"

namespace exchange {
    class ModelAdapter;

    struct PremiumMarketSignal {
        std::string signal;
        std::uint32_t confidence{};
        std::string reason;

        bool operator==(const PremiumMarketSignal&) const = default;
    };

    struct RiskGuidance {
        Amount available_quote{};
        Price local_best_ask{};
        std::uint16_t risk_budget_bps{};
        Quantity affordable_quantity{};
        Quantity risk_budget_quantity{};
        Amount objective_remaining{};
        Quantity max_recommended_quantity{};
        std::string reason;

        bool operator==(const RiskGuidance&) const = default;
    };

    // Scenario-level advisory role only. It has no access to mutable economic
    // state and cannot approve, reject, or execute an order.
    class DeterministicRiskAgent {
    public:
        explicit DeterministicRiskAgent(std::uint16_t risk_budget_bps);

        [[nodiscard]] RiskGuidance assess(
            const AgentObservation& observation) const;

    private:
        std::uint16_t risk_budget_bps_;
    };

    class PremiumInformationService {
    public:
        PremiumInformationService(
            ExternalPaymentRequirement payment_requirement,
            PremiumMarketSignal signal);

        [[nodiscard]] const ExternalPaymentRequirement& payment_requirement()
            const noexcept;

        // This is preview authorization only. It does not represent payment,
        // signing, settlement, or transfer of funds.
        [[nodiscard]] std::optional<PremiumMarketSignal> access_after_preview(
            const ExternalPaymentPreview& preview) const;

    private:
        ExternalPaymentRequirement payment_requirement_;
        PremiumMarketSignal signal_;
    };

    class PremiumSignalPolicy {
    public:
        virtual ~PremiumSignalPolicy() = default;

        [[nodiscard]] virtual AgentAction decide(
            const AgentObservation& observation,
            const PremiumMarketSignal& premium_signal,
            const RiskGuidance& risk_guidance) const = 0;

        // Episode context is presentation/orchestration input only. The
        // default preserves existing policies which do not need it.
        [[nodiscard]] virtual AgentAction decide_with_episode_context(
            const AgentObservation& observation,
            const PremiumMarketSignal& premium_signal,
            const RiskGuidance& risk_guidance,
            std::string_view episode_context) const;
    };

    class ModelPremiumSignalPolicy final : public PremiumSignalPolicy {
    public:
        explicit ModelPremiumSignalPolicy(
            ModelAgentPolicy& model_policy) noexcept;

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation,
            const PremiumMarketSignal& premium_signal,
            const RiskGuidance& risk_guidance) const override;

        [[nodiscard]] AgentAction decide_with_episode_context(
            const AgentObservation& observation,
            const PremiumMarketSignal& premium_signal,
            const RiskGuidance& risk_guidance,
            std::string_view episode_context) const override;

    private:
        ModelAgentPolicy& model_policy_;
    };

    using PaymentPreviewFunction = std::function<ExternalPaymentPreview(
        const ExternalPaymentRequirement&)>;

    struct HackathonDemoConfig {
        AgentId trader_agent_id{};
        AgentId analyst_agent_id{};
        AgentId risk_agent_id{};
        AssetTargetObjective trader_objective;

        bool operator==(const HackathonDemoConfig&) const = default;
    };

    struct HackathonDemoTurn {
        ExternalPaymentPreview preview;
        bool preview_authorized{};
        std::optional<PremiumMarketSignal> premium_signal;
        RiskGuidance risk_guidance;
        AgentObservation observation;
        AgentAction action;
        AgentActionResult action_result;
        ObjectiveProgress trader_objective;

        bool operator==(const HackathonDemoTurn&) const = default;
    };

    enum class HackathonDemoScenarioKind : std::uint8_t {
        Normal,
        AgentError,
    };

    struct HackathonDemoExecutionSnapshot {
        Balance trader_base_before;
        Balance trader_quote_before;
        Balance analyst_base_before;
        Balance analyst_quote_before;
        Balance trader_base_after;
        Balance trader_quote_after;
        Balance analyst_base_after;
        Balance analyst_quote_after;
        std::optional<Trade> trade;
        std::size_t ledger_entry_count{};
        bool balances_unchanged{};
        bool ledger_unchanged{};
        bool no_trader_reservation{};
        bool analyst_resting_order_unchanged{};

        bool operator==(const HackathonDemoExecutionSnapshot&) const = default;
    };

    // Hackathon-only presentation snapshot. It is downstream of the existing
    // scenario and carries no authority over matching, accounting, or replay.
    struct HackathonDemoSnapshot {
        HackathonDemoScenarioKind scenario{HackathonDemoScenarioKind::Normal};
        ExternalPaymentRequirement payment_requirement;
        HackathonDemoTurn turn;
        HackathonDemoExecutionSnapshot execution;
        std::size_t payment_preview_calls{};
        std::size_t model_calls_original{};
        std::size_t model_calls_replay{};
        std::size_t payment_service_calls_replay{};
        bool replay_available{};
        bool balance_parity{};
        bool ledger_parity{};
        bool objective_parity{};
        bool order_parity{};
        bool trade_parity{};

        bool operator==(const HackathonDemoSnapshot&) const = default;
    };

    // Runs the existing three-Agent scenario and returns presentation-only
    // evidence. Normal mode calls the supplied model exactly through the
    // existing ModelPremiumSignalPolicy; AgentError intentionally submits BUY
    // 10 to demonstrate authoritative core rejection.
    [[nodiscard]] HackathonDemoSnapshot run_hackathon_demo_snapshot(
        HackathonDemoScenarioKind scenario,
        ModelAdapter* model_adapter = nullptr);

    enum class HackathonSimulationStatus : std::uint8_t {
        Idle,
        Running,
        StopRequested,
        GoalAchieved,
        UserStopped,
        MaxRounds,
        Error,
    };

    enum class HackathonReplayStatus : std::uint8_t {
        NotRun,
        Running,
        Exact,
        Mismatch,
    };

    struct HackathonSimulationConfig {
        Amount target_base{5};
        std::size_t max_rounds{12};
        std::uint64_t seed{0x5EED42};

        bool operator==(const HackathonSimulationConfig&) const = default;
    };

    // Presentation events are emitted after an authoritative scenario phase;
    // they do not command or alter the underlying financial world.
    struct HackathonSimulationActivity {
        std::size_t sequence{};
        std::size_t round{};
        std::string role;
        std::string detail;

        bool operator==(const HackathonSimulationActivity&) const = default;
    };

    struct HackathonMatchEngineEvidence {
        std::optional<Order> incoming_order;
        std::optional<Price> best_bid_before;
        std::optional<Price> best_ask_before;
        std::optional<Trade> trade;
        std::vector<Trade> trades;
        Quantity maker_remaining_quantity{};
        std::size_t maker_orders_consumed{};
        bool multi_level_taker{};
        bool partial_fill{};
        std::size_t active_order_count{};
        bool price_time_priority{true};
        bool reservation_consumed{};
        bool balances_unchanged{};
        bool ledger_unchanged{};
        bool no_residual_reservation{};

        bool operator==(const HackathonMatchEngineEvidence&) const = default;
    };

    // Score is hackathon-only episode evaluation. It never participates in
    // account balances, reservations, matching, Ledger, or replay parity.
    struct HackathonEpisodeScore {
        std::int64_t total{};
        std::int64_t objective_progress_points{};
        std::int64_t accepted_action_points{};
        std::int64_t risk_compliant_points{};
        std::int64_t useful_hold_points{};
        std::int64_t rejected_action_points{};
        std::int64_t risk_violation_points{};

        bool operator==(const HackathonEpisodeScore&) const = default;
    };

    struct HackathonRoundEvidence {
        std::size_t round{};
        ExternalMarketSnapshot external_market;
        PremiumMarketSignal analyst_signal;
        std::string analyst_public_reason;
        RiskGuidance risk_guidance;
        AgentAction action;
        AgentActionResult action_result;
        std::vector<Trade> trades;
        std::optional<Order> resting_order;
        Amount reserved_quote_after{};
        ObjectiveProgress objective_after;
        std::int64_t score_before{};
        std::int64_t score_delta{};
        std::int64_t score_after{};
        std::vector<std::string> score_reasons;

        bool operator==(const HackathonRoundEvidence& other) const {
            const bool same_resting_order = resting_order.has_value()
                == other.resting_order.has_value()
                && (!resting_order.has_value()
                    || (resting_order->id == other.resting_order->id
                        && resting_order->side == other.resting_order->side
                        && resting_order->type == other.resting_order->type
                        && resting_order->price == other.resting_order->price
                        && resting_order->quantity == other.resting_order->quantity
                        && resting_order->timestamp == other.resting_order->timestamp));
            return round == other.round
                && external_market == other.external_market
                && analyst_signal == other.analyst_signal
                && analyst_public_reason == other.analyst_public_reason
                && risk_guidance == other.risk_guidance
                && action == other.action
                && action_result == other.action_result
                && trades == other.trades
                && same_resting_order
                && reserved_quote_after == other.reserved_quote_after
                && objective_after == other.objective_after
                && score_before == other.score_before
                && score_delta == other.score_delta
                && score_after == other.score_after
                && score_reasons == other.score_reasons;
        }
    };

    struct HackathonSimulationSummary {
        std::string end_reason;
        std::uint64_t seed{};
        std::size_t rounds_completed{};
        std::size_t deepseek_calls{};
        std::size_t analyst_service_accesses{};
        std::size_t trader_decisions{};
        std::size_t orders_submitted{};
        std::size_t accepted_actions{};
        std::size_t rejected_actions{};
        std::size_t held_actions{};
        std::size_t trades{};
        Amount base_acquired{};
        // Sum of actual Trade quote amounts. It excludes quote that remains
        // reserved for a resting BUY order.
        Amount quote_spent{};
        Amount current_reserved_quote{};
        Amount filled_base_quantity{};
        Amount final_base{};
        Amount objective_target{};
        std::size_t ledger_entries{};
        std::size_t active_orders{};
        std::size_t invalid_state_mutations{};
        HackathonEpisodeScore score;

        bool operator==(const HackathonSimulationSummary&) const = default;
    };

    struct HackathonReplayStage {
        std::string name;
        std::string state;

        bool operator==(const HackathonReplayStage&) const = default;
    };

    // Read-only presentation snapshot captured from each completed economic
    // world. It does not participate in replay execution or parity decisions.
    struct HackathonReplayWorldState {
        Balance trader_base;
        Balance trader_quote;
        std::size_t trade_records{};
        Amount filled_base_quantity{};
        Amount executed_quote_amount{};
        std::size_t active_orders{};
        std::size_t ledger_entries{};
        ObjectiveProgress objective;

        bool operator==(const HackathonReplayWorldState&) const = default;
    };

    struct HackathonReplayEvidence {
        HackathonReplayStatus status{HackathonReplayStatus::NotRun};
        std::vector<HackathonReplayStage> stages;
        std::size_t deepseek_calls_original{};
        std::size_t deepseek_calls_replay{};
        std::size_t payment_service_calls_original{};
        std::size_t payment_service_calls_replay{};
        std::vector<std::string> captured_economic_inputs;
        std::optional<HackathonReplayWorldState> original_final_state;
        std::optional<HackathonReplayWorldState> replay_final_state;
        bool balance_parity{};
        bool ledger_parity{};
        bool objective_parity{};
        bool order_parity{};
        bool trade_parity{};

        bool operator==(const HackathonReplayEvidence&) const = default;
    };

    struct HackathonSimulationSnapshot {
        HackathonDemoScenarioKind scenario{HackathonDemoScenarioKind::Normal};
        HackathonSimulationStatus status{HackathonSimulationStatus::Idle};
        HackathonSimulationConfig config;
        std::size_t rounds_completed{};
        ObjectiveProgress objective;
        Balance trader_base;
        Balance trader_quote;
        Balance analyst_base;
        Balance analyst_quote;
        std::size_t ledger_entries{};
        std::size_t active_orders{};
        std::size_t trade_count{};
        std::size_t rejected_actions{};
        std::optional<HackathonDemoTurn> last_turn;
        HackathonMatchEngineEvidence match_engine;
        HackathonEpisodeScore score;
        std::vector<HackathonRoundEvidence> round_evidence;
        std::vector<HackathonSimulationActivity> activities;
        std::optional<HackathonSimulationSummary> summary;
        HackathonReplayEvidence replay;

        bool operator==(const HackathonSimulationSnapshot&) const = default;
    };

    // A hackathon-only orchestration object. Calls are synchronous and each
    // advance() completes one whole Agent/economic round before returning.
    class HackathonSimulation {
    public:
        HackathonSimulation(
            HackathonDemoScenarioKind scenario,
            ModelAdapter* model_adapter = nullptr,
            HackathonSimulationConfig config = {});
        ~HackathonSimulation();

        HackathonSimulation(const HackathonSimulation&) = delete;
        HackathonSimulation& operator=(const HackathonSimulation&) = delete;
        HackathonSimulation(HackathonSimulation&&) = delete;
        HackathonSimulation& operator=(HackathonSimulation&&) = delete;

        void start();
        void advance();
        void request_stop();
        void start_replay();
        void advance_replay();

        [[nodiscard]] const HackathonSimulationSnapshot& snapshot() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Captures only deterministic economic input selected during the original
    // cognition phase. It contains no payment, provider, or model data.
    struct HackathonEconomicReplayEvidence {
        AccountId account_id{};
        Order order;
        SubmitResult expected_result{SubmitResult::InvalidOrder};
    };

    [[nodiscard]] HackathonEconomicReplayEvidence
    capture_hackathon_economic_replay_evidence(
        const AgentRegistry& registry,
        AgentId agent_id,
        const AgentAction& action,
        const AgentActionResult& result);

    // Re-enters the existing account-backed execution path. It does not call
    // payment, information, model, or any other external provider.
    [[nodiscard]] SubmitResult replay_hackathon_economic_evidence(
        const HackathonEconomicReplayEvidence& evidence,
        ExecutionCoordinator& execution_coordinator);

    // Thin hackathon-only composition layer. Financial mutations remain behind
    // AgentActionGateway and the existing ExecutionCoordinator path.
    class HackathonDemoScenario {
    public:
        HackathonDemoScenario(
            HackathonDemoConfig config,
            PremiumInformationService premium_service,
            const AgentRegistry& registry,
            const AgentObservationService& observations,
            AgentActionGateway& actions,
            const DeterministicRiskAgent& risk_agent,
            const PremiumSignalPolicy& trader_policy);

        HackathonDemoScenario(const HackathonDemoScenario&) = delete;
        HackathonDemoScenario& operator=(const HackathonDemoScenario&) = delete;
        HackathonDemoScenario(HackathonDemoScenario&&) = delete;
        HackathonDemoScenario& operator=(HackathonDemoScenario&&) = delete;

        [[nodiscard]] HackathonDemoTurn run_once(
            const PaymentPreviewFunction& preview_payment,
            std::optional<ExternalMarketSnapshot> external_market =
                std::nullopt,
            std::string_view episode_context = {});

    private:
        HackathonDemoConfig config_;
        PremiumInformationService premium_service_;
        const AgentObservationService& observations_;
        AgentActionGateway& actions_;
        const DeterministicRiskAgent& risk_agent_;
        const PremiumSignalPolicy& trader_policy_;
        bool run_called_{};
    };
}  // namespace exchange
