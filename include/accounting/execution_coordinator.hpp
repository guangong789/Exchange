#pragma once

#include "accounting/account.hpp"
#include "core/types.hpp"
#include "matching/order.hpp"

#include "accounting/account_store.hpp"
#include "matching/event_collector.hpp"
#include "accounting/financial_conversion.hpp"
#include "accounting/ledger.hpp"
#include "matching/matching_engine.hpp"
#include "accounting/order_reservation_store.hpp"

namespace exchange {
    struct OrderAdmissionRequest {
        AccountId account_id{};
        Order order;
    };

    enum class SubmitResult {
        Accepted,
        AccountNotFound,
        InsufficientFunds,
        DuplicateOrder,
        InvalidOrder,
        CounterpartyNotAccountBacked,
    };

    enum class CancelResult {
        Cancelled,
        AccountNotFound,
        NotFound,
        NotOwner,
    };

    class ExecutionCoordinator {
    public:
        ExecutionCoordinator(
            InstrumentContext instrument,
            AccountStore& accounts,
            OrderReservationStore& reservations,
            MatchingEngine& matching_engine,
            EventCollector& events,
            Ledger& ledger);

        ExecutionCoordinator(const ExecutionCoordinator&) = delete;
        ExecutionCoordinator& operator=(const ExecutionCoordinator&) = delete;
        ExecutionCoordinator(ExecutionCoordinator&&) = delete;
        ExecutionCoordinator& operator=(ExecutionCoordinator&&) = delete;

        [[nodiscard]] SubmitResult submit_order(
            const OrderAdmissionRequest& request);

        [[nodiscard]] CancelResult cancel_order(
            AccountId requester,
            OrderId order_id);

    private:
        const InstrumentContext instrument_;
        AccountStore& accounts_;
        OrderReservationStore& reservations_;
        MatchingEngine& matching_engine_;
        EventCollector& events_;
        Ledger& ledger_;
    };
}  // namespace exchange
