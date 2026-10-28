#pragma once

#include "accounting/financial_conversion.hpp"
#include "durability/command_journal.hpp"
#include "execution/trading_command_applier.hpp"
#include "execution/execution_runtime_status.hpp"
#include "execution/execution_sequencer.hpp"
#include "execution/trading_request.hpp"

namespace exchange {
    class TradingRuntime;

    class TradingRequestExecutor {
    public:
        TradingRequestExecutor(
            InstrumentContext instrument,
            ExecutionCoordinator& execution_coordinator,
            EventCollector& events,
            ExecutionSequencer& sequencer,
            ExecutionCommandJournal* command_journal = nullptr,
            ExecutionRuntimeStatus* runtime_status = nullptr) noexcept;

        TradingRequestExecutor(const TradingRequestExecutor&) = delete;
        TradingRequestExecutor& operator=(const TradingRequestExecutor&) = delete;
        TradingRequestExecutor(TradingRequestExecutor&&) = delete;
        TradingRequestExecutor& operator=(TradingRequestExecutor&&) = delete;

        [[nodiscard]] TradingResponse execute(const TradingRequest& request);
        // Unexpected journal/apply failures poison this executor (and shared
        // runtime) in either mode before rethrowing the original exception.
        // Typed business rejections leave it usable. No global rollback is
        // promised after matching starts; discard/recover a poisoned runtime.
        [[nodiscard]] bool poisoned() const noexcept;
        [[nodiscard]] bool durable_processing_started() const noexcept;

    private:
        friend class TradingRuntime;

        void attach_command_journal(
            ExecutionCommandJournal& command_journal) noexcept;

        const InstrumentContext instrument_;
        TradingCommandApplier command_applier_;
        EventCollector& events_;
        ExecutionSequencer& sequencer_;
        ExecutionCommandJournal* command_journal_{};
        ExecutionRuntimeStatus* runtime_status_{};
        bool poisoned_{};
        bool durable_processing_started_{};
    };
}  // namespace exchange
