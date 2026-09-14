#pragma once

#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/agent_execution_adapter.hpp"
#include "execution/contract_request_executor.hpp"
#include "execution/trading_request_executor.hpp"

namespace exchange {
    class TradingRequestAgentExecutionAdapter final
        : public AgentExecutionAdapter {
    public:
        TradingRequestAgentExecutionAdapter(
            const AgentRegistry& registry,
            TradingRequestExecutor& executor,
            ContractRequestExecutor& contract_executor,
            RequestId first_request_id = 1);

        TradingRequestAgentExecutionAdapter(
            const TradingRequestAgentExecutionAdapter&) = delete;
        TradingRequestAgentExecutionAdapter& operator=(
            const TradingRequestAgentExecutionAdapter&) = delete;

        [[nodiscard]] AgentActionResult execute(
            AgentId agent_id,
            const AgentAction& action) override;

    private:
        [[nodiscard]] RequestId allocate_request_id();

        const AgentRegistry& registry_;
        TradingRequestExecutor& executor_;
        ContractRequestExecutor& contract_executor_;
        RequestId next_request_id_;
    };
}  // namespace exchange
