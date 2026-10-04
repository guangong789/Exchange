#pragma once

#include "accounting/ledger.hpp"
#include "accounting/order_reservation_store.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_request.hpp"

#include <optional>
#include <string>
#include <vector>

namespace exchange::diagnostics {
    struct OpposingOrderEvidence {
        Order order;
        bool executable{};
    };

    struct LedgerTradeEvidence {
        LedgerSequence ledger_sequence{};
        Trade trade;
    };

    struct PartialFillEvidence {
        RequestId request_id{};
        AccountId account_id{};
        Order submitted_order;
        WalSequence wal_sequence{};
        std::vector<OpposingOrderEvidence> opposing_orders_before;
        Quantity total_executable_quantity_before{};
        TradingResult execution_result{TradingResult::InvalidRequest};
        std::vector<Event> execution_events;
        std::vector<LedgerTradeEvidence> trades;
        Quantity matched_quantity{};
        std::optional<Order> resting_order_after;
        std::optional<OrderReservation> reservation_after;
        std::optional<Price> best_ask_after;
    };

    [[nodiscard]] PartialFillEvidence make_partial_fill_evidence();
    void validate_partial_fill_evidence(const PartialFillEvidence& evidence);
    [[nodiscard]] std::string partial_fill_evidence_json(
        const PartialFillEvidence& evidence);
}  // namespace exchange::diagnostics
