#pragma once

#include "exchange/account_store.hpp"
#include "exchange/event_collector.hpp"
#include "exchange/financial_conversion.hpp"
#include "exchange/matching_engine.hpp"
#include "exchange/order_reservation_store.hpp"

namespace exchange {
    struct OrderAdmissionRequest {
        AccountId account_id{};
        Order order;
    };

    enum class SubmitResult {
        Accepted,
        InsufficientFunds,
        DuplicateOrder,
        InvalidOrder,
        CounterpartyNotAccountBacked,
    };

    enum class CancelResult {
        Cancelled,
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
            EventCollector& events);

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
    };
}  // namespace exchange
