#pragma once

#include "diagnostics/historical_order_evidence.hpp"
#include "durability/execution_recovery.hpp"

#include <span>

namespace exchange::diagnostics::detail {
    // Internal assembly seam: require one ordered outcome per trading record.
    // No command is applied here; results come from completed recovery.
    [[nodiscard]] std::optional<HistoricalOrderEvidence>
    associate_recovered_order_outcomes(
        OrderId order_id,
        std::span<const WalRecord> records,
        std::span<const RecoveredTradingOutcome> outcomes);
}  // namespace exchange::diagnostics::detail
