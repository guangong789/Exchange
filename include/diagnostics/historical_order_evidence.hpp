#pragma once

#include "accounting/ledger.hpp"
#include "accounting/order_reservation_store.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_request.hpp"

#include <optional>
#include <string>
#include <vector>

namespace exchange::diagnostics {
    struct HistoricalEvidenceConfig {
        InstrumentContext instrument;
        TradingBootstrapConfig bootstrap;
    };

    struct HistoricalTradeEvidence {
        LedgerSequence ledger_sequence{};
        Trade trade;
    };

    struct HistoricalCancelAttemptEvidence {
        CancelExecutionCommand command;
        WalSequence wal_sequence{};
        TradingResult recovered_result{TradingResult::InvalidRequest};
    };

    enum class HistoricalOrderState {
        Resting,
        FullyFilled,
        NotResting,
    };

    struct HistoricalOrderEvidence {
        // The WAL proves command admission, not successful business execution
        // or response delivery. Submit records contain the assigned OrderId.
        SubmitExecutionCommand submission;
        WalSequence submission_wal_sequence{};
        // Outcomes are recomputed by production recovery, not stored responses.
        TradingResult submission_recovered_result{TradingResult::InvalidRequest};
        std::vector<HistoricalCancelAttemptEvidence> cancel_attempts;
        std::vector<HistoricalTradeEvidence> trades;
        Quantity matched_quantity{};
        HistoricalOrderState final_state{HistoricalOrderState::NotResting};
        std::optional<Order> resting_order;
        std::optional<OrderReservation> reservation;
        WalSequence recovered_through_wal_sequence{};
        bool ignored_torn_tail{};
    };

    // Config JSON has instrument and bootstrap.accounts, using the exact
    // InstrumentContext / BootstrapAccount / BootstrapBalance field names.
    [[nodiscard]] HistoricalEvidenceConfig load_historical_evidence_config(
        const std::string& path);

    // Offline only: read-lock a regular source WAL, validate its bytes, and
    // perform complete production recovery on a private temporary copy.
    // The source is never opened writable or repaired. A live writer's lock
    // causes failure. nullopt means no durable submit command has this OrderId.
    [[nodiscard]] std::optional<HistoricalOrderEvidence>
    reconstruct_historical_order_evidence(
        const HistoricalEvidenceConfig& config,
        const std::string& offline_wal_path,
        OrderId order_id);

    [[nodiscard]] std::string historical_order_evidence_json(
        const HistoricalOrderEvidence& evidence);
}  // namespace exchange::diagnostics
