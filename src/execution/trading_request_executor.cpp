#include "exchange/execution/trading_request_executor.hpp"

#include "exchange/execution/trading_request_admission.hpp"

#include <optional>
#include <stdexcept>
#include <variant>

namespace exchange {
    TradingRequestExecutor::TradingRequestExecutor(
        InstrumentContext instrument,
        ExecutionCoordinator& execution_coordinator,
        EventCollector& events,
        ExecutionSequencer& sequencer,
        ExecutionCommandJournal* command_journal) noexcept
        : instrument_(instrument),
          command_applier_(execution_coordinator, events),
          events_(events),
          sequencer_(sequencer),
          command_journal_(command_journal) {}

    TradingResponse TradingRequestExecutor::execute(
        const TradingRequest& request) {
        if (poisoned_) {
            throw std::logic_error("trading request executor is poisoned");
        }
        if (command_journal_ != nullptr) {
            durable_processing_started_ = true;
        }
        events_.clear();
        ExecutionAdmissionResult admission = admit_trading_request(
            request,
            instrument_,
            sequencer_);
        if (const auto* rejection = std::get_if<TradingResult>(&admission)) {
            return TradingResponse{
                request.request_id,
                *rejection,
                {},
                std::nullopt};
        }
        const ExecutionCommand& command =
            std::get<ExecutionCommand>(admission);
        if (command_journal_ == nullptr) {
            return command_applier_.apply(command);
        }

        try {
            command_journal_->append(command);
            return command_applier_.apply(command);
        } catch (...) {
            poisoned_ = true;
            throw;
        }
    }

    bool TradingRequestExecutor::poisoned() const noexcept {
        return poisoned_;
    }

    bool TradingRequestExecutor::durable_processing_started() const noexcept {
        return durable_processing_started_;
    }

    void TradingRequestExecutor::attach_command_journal(
        ExecutionCommandJournal& command_journal) noexcept {
        command_journal_ = &command_journal;
    }
}  // namespace exchange
