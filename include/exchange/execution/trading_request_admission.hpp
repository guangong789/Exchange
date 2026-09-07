#pragma once

#include "exchange/accounting/financial_conversion.hpp"
#include "exchange/execution/execution_command.hpp"
#include "exchange/execution/execution_sequencer.hpp"
#include "exchange/execution/trading_request.hpp"

#include <variant>

namespace exchange {
    using ExecutionAdmissionResult = std::variant<
        ExecutionCommand,
        TradingResult>;

    [[nodiscard]] ExecutionAdmissionResult admit_trading_request(
        const TradingRequest& request,
        const InstrumentContext& instrument,
        ExecutionSequencer& sequencer);
}  // namespace exchange
