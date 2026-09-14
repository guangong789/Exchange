#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "agent/domain/action_validation.hpp"
#include "agent/domain/agent.hpp"
#include "agent/domain/economic_constraints.hpp"
#include "agent/domain/external_market.hpp"
#include "agent/domain/utility.hpp"

namespace exchange {
    enum class AgentTurnStatus {
        DecisionFailed,
        StructuralValidationRejected,
        EconomicConstraintRejected,
        ExecutionRejected,
        Executed,
        Held,
    };

    struct AgentStateSummary {
        std::optional<Balance> base_balance;
        std::optional<Balance> quote_balance;
        std::uint64_t active_order_count{};

        bool operator==(const AgentStateSummary&) const = default;
    };

    struct AgentTurnRecord {
        std::uint64_t step{};
        std::int64_t local_timestamp_ms{};
        AgentId agent_id{};
        AgentObservation observation;
        std::optional<ExternalMarketState> external_market_context;
        AgentStateSummary pre_state;
        AgentStateSummary post_state;
        std::optional<AgentAction> action;
        std::optional<AgentActionValidationResult> validation;
        std::optional<AgentEconomicConstraintResult> economic_constraint;
        std::optional<AgentActionResult> execution_result;
        std::optional<AgentUtilityBreakdown> utility_before;
        std::optional<AgentUtilityBreakdown> utility_after;
        std::optional<UtilityValue> utility_delta;
        std::optional<ContractState> contract_state_after_action;
        AgentTurnStatus status{AgentTurnStatus::DecisionFailed};

        bool operator==(const AgentTurnRecord&) const = default;
    };

    struct PerAgentExperimentMetrics {
        AgentId agent_id{};
        std::uint64_t turns{};
        std::uint64_t proposed_submits{};
        std::uint64_t proposed_cancels{};
        std::uint64_t contract_proposals{};
        std::uint64_t contract_accepts{};
        std::uint64_t contract_rejects{};
        std::uint64_t contract_fulfillment_attempts{};
        std::uint64_t successful_contract_fulfillments{};
        std::uint64_t contract_fulfillment_rejections{};
        std::uint64_t contract_settlement_attempts{};
        std::uint64_t successful_contract_settlements{};
        std::uint64_t contract_settlement_rejections{};
        std::uint64_t successful_contract_actions{};
        std::uint64_t contract_execution_rejections{};
        std::uint64_t holds{};
        std::uint64_t decision_failures{};
        std::uint64_t structural_rejections{};
        std::uint64_t economic_constraint_rejections{};
        std::uint64_t execution_rejections{};
        std::uint64_t successful_executions{};
        std::uint64_t proposed_buys{};
        std::uint64_t proposed_sells{};
        Quantity total_proposed_quantity{};
        Quantity total_accepted_quantity{};
        UtilityValue cumulative_utility_delta{};
        std::uint64_t positive_utility_turns{};
        std::uint64_t negative_utility_turns{};
        std::uint64_t zero_utility_turns{};
        std::optional<UtilityValue> final_utility;
        std::optional<UtilityValue> best_utility_delta;
        std::optional<UtilityValue> worst_utility_delta;
        AgentStateSummary final_state;

        bool operator==(const PerAgentExperimentMetrics&) const = default;
    };

    struct SocietyExperimentMetrics {
        std::uint64_t total_turns{};
        std::uint64_t total_successful_executions{};
        std::uint64_t total_structural_rejections{};
        std::uint64_t total_economic_constraint_rejections{};
        std::uint64_t total_exchange_rejections{};
        std::uint64_t total_buys{};
        std::uint64_t total_sells{};
        std::uint64_t total_holds{};
        std::uint64_t total_contract_proposals{};
        std::uint64_t total_contract_accepts{};
        std::uint64_t total_contract_rejects{};
        std::uint64_t total_contract_fulfillment_attempts{};
        std::uint64_t total_successful_contract_fulfillments{};
        std::uint64_t total_contract_fulfillment_rejections{};
        std::uint64_t total_contract_settlement_attempts{};
        std::uint64_t total_successful_contract_settlements{};
        std::uint64_t total_contract_settlement_rejections{};
        std::uint64_t total_successful_contract_actions{};
        std::uint64_t total_contract_execution_rejections{};

        bool operator==(const SocietyExperimentMetrics&) const = default;
    };

    struct ContractPairInteractionMetrics {
        std::uint64_t proposal_attempts{};
        std::uint64_t accepted_contracts{};

        bool operator==(const ContractPairInteractionMetrics&) const = default;
    };

    class AgentExperimentMetrics {
    public:
        void record(const AgentTurnRecord& turn);

        [[nodiscard]] const PerAgentExperimentMetrics* find_agent(
            AgentId agent_id) const noexcept;
        [[nodiscard]] const std::map<AgentId, PerAgentExperimentMetrics>&
            per_agent() const noexcept;
        [[nodiscard]] const std::map<
            std::pair<AgentId, AgentId>,
            ContractPairInteractionMetrics>&
        contract_pair_interactions() const noexcept;
        [[nodiscard]] SocietyExperimentMetrics society() const;

    private:
        std::map<AgentId, PerAgentExperimentMetrics> per_agent_;
        std::map<
            std::pair<AgentId, AgentId>,
            ContractPairInteractionMetrics>
            contract_pair_interactions_;
    };
}  // namespace exchange
