#pragma once

#include "accounting/execution_coordinator.hpp"
#include "execution/execution_command.hpp"
#include "execution/trading_request.hpp"
#include "matching/event_collector.hpp"

namespace exchange {
    class TradingCommandApplier {
    public:
        TradingCommandApplier(
            ExecutionCoordinator& execution_coordinator,
            EventCollector& events) noexcept;

        TradingCommandApplier(const TradingCommandApplier&) = delete;
        TradingCommandApplier& operator=(
            const TradingCommandApplier&) = delete;
        TradingCommandApplier(TradingCommandApplier&&) = delete;
        TradingCommandApplier& operator=(TradingCommandApplier&&) = delete;

        // An unexpected exception means application may have partially
        // mutated memory. Callers must treat the runtime as unsafe to reuse.
        [[nodiscard]] TradingResponse apply(const ExecutionCommand& command);

    private:
        ExecutionCoordinator& execution_coordinator_;
        EventCollector& events_;
    };
}  // namespace exchange
