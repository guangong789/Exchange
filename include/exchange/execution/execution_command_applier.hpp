#pragma once

#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/execution/execution_command.hpp"
#include "exchange/execution/trading_request.hpp"
#include "exchange/matching/event_collector.hpp"

namespace exchange {
    class ExecutionCommandApplier {
    public:
        ExecutionCommandApplier(
            ExecutionCoordinator& execution_coordinator,
            EventCollector& events) noexcept;

        ExecutionCommandApplier(const ExecutionCommandApplier&) = delete;
        ExecutionCommandApplier& operator=(
            const ExecutionCommandApplier&) = delete;
        ExecutionCommandApplier(ExecutionCommandApplier&&) = delete;
        ExecutionCommandApplier& operator=(ExecutionCommandApplier&&) = delete;

        // An unexpected exception means application may have partially
        // mutated memory. Callers must treat the runtime as unsafe to reuse.
        [[nodiscard]] TradingResponse apply(const ExecutionCommand& command);

    private:
        ExecutionCoordinator& execution_coordinator_;
        EventCollector& events_;
    };
}  // namespace exchange
