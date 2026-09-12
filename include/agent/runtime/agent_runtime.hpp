#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "accounting/financial_conversion.hpp"
#include "agent/domain/action_validation.hpp"
#include "agent/domain/agent.hpp"
#include "agent/domain/external_market_feed.hpp"
#include "agent/exchange/agent_execution_adapter.hpp"
#include "agent/exchange/agent_observation_service.hpp"

namespace exchange {
    struct AgentRuntimeParticipant {
        AgentId agent_id{};
        // Non-owning. The provider must outlive AgentRuntime.
        const AgentDecisionProvider* decision_provider{};
        std::optional<AssetTargetObjective> objective;
    };

    enum class AgentTurnStatus {
        Executed,
        ActionRejected,
        DecisionFailed,
    };

    struct AgentTurnRecord {
        std::uint64_t step{};
        AgentId agent_id{};
        AgentObservation observation;
        std::optional<AgentAction> action;
        std::optional<AgentActionValidationResult> validation;
        std::optional<AgentActionResult> execution_result;
        AgentTurnStatus status{AgentTurnStatus::DecisionFailed};

        bool operator==(const AgentTurnRecord&) const = default;
    };

    class AgentRuntime {
    public:
        AgentRuntime(
            std::vector<AgentRuntimeParticipant> participants,
            const AgentObservationService& observation_service,
            AgentExecutionAdapter& execution_adapter,
            InstrumentContext instrument,
            const ExternalMarketFeed* external_market_feed = nullptr);

        AgentRuntime(const AgentRuntime&) = delete;
        AgentRuntime& operator=(const AgentRuntime&) = delete;
        AgentRuntime(AgentRuntime&&) = delete;
        AgentRuntime& operator=(AgentRuntime&&) = delete;

        void run_step();
        void run_step_at(std::int64_t local_now_ms);

        [[nodiscard]] std::uint64_t current_step() const noexcept;
        [[nodiscard]] const std::vector<AgentTurnRecord>& trace()
            const noexcept;

    private:
        const std::vector<AgentRuntimeParticipant> participants_;
        const AgentObservationService& observation_service_;
        AgentExecutionAdapter& execution_adapter_;
        const InstrumentContext instrument_;
        const ExternalMarketFeed* external_market_feed_;
        std::vector<AgentTurnRecord> trace_;
        std::uint64_t current_step_{};
    };
}  // namespace exchange
