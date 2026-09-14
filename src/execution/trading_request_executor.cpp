#include "execution/trading_request_executor.hpp"

#include "execution/trading_request_admission.hpp"

#include <optional>
#include <stdexcept>
#include <variant>

namespace exchange {
    TradingRequestExecutor::TradingRequestExecutor(
        InstrumentContext instrument,
        ExecutionCoordinator& execution_coordinator,
        EventCollector& events,
        ExecutionSequencer& sequencer,
        ExecutionCommandJournal* command_journal,
        ExecutionRuntimeStatus* runtime_status) noexcept
        : instrument_(instrument),
          command_applier_(execution_coordinator, events),
          events_(events),
          sequencer_(sequencer),
          command_journal_(command_journal),
          runtime_status_(runtime_status) {}

    TradingResponse TradingRequestExecutor::execute(
        const TradingRequest& request) {
        if (poisoned()) {
            throw std::logic_error("trading request executor is poisoned");
        }
        if (command_journal_ != nullptr) {
            durable_processing_started_ = true;
            if (runtime_status_ != nullptr) {
                runtime_status_->durable_processing_started = true;
            }
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
            if (runtime_status_ != nullptr) {
                runtime_status_->poisoned = true;
            }
            throw;
        }
    }

    bool TradingRequestExecutor::poisoned() const noexcept {
        return poisoned_
            || (runtime_status_ != nullptr && runtime_status_->poisoned);
    }

    bool TradingRequestExecutor::durable_processing_started() const noexcept {
        return durable_processing_started_
            || (runtime_status_ != nullptr
                && runtime_status_->durable_processing_started);
    }

    void TradingRequestExecutor::attach_command_journal(
        ExecutionCommandJournal& command_journal) noexcept {
        command_journal_ = &command_journal;
    }
}  // namespace exchange
