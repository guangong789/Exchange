#pragma once

#include "exchange/accounting/account_store.hpp"
#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/accounting/financial_conversion.hpp"
#include "exchange/accounting/ledger.hpp"
#include "exchange/accounting/order_reservation_store.hpp"
#include "exchange/durability/execution_wal_writer.hpp"
#include "exchange/execution/execution_sequencer.hpp"
#include "exchange/execution/trading_bootstrap.hpp"
#include "exchange/execution/trading_request_executor.hpp"
#include "exchange/matching/event_collector.hpp"
#include "exchange/matching/matching_engine.hpp"
#include "exchange/matching/order_book.hpp"

#include <memory>
#include <string>

namespace exchange {
    class TradingRuntime {
    public:
        explicit TradingRuntime(InstrumentContext instrument);

        [[nodiscard]] static std::unique_ptr<TradingRuntime> create_durable(
            InstrumentContext instrument,
            std::string wal_path,
            const TradingBootstrapConfig& bootstrap);

        TradingRuntime(const TradingRuntime&) = delete;
        TradingRuntime& operator=(const TradingRuntime&) = delete;
        TradingRuntime(TradingRuntime&&) = delete;
        TradingRuntime& operator=(TradingRuntime&&) = delete;

        [[nodiscard]] TradingRequestExecutor& executor() noexcept;

        [[nodiscard]] const InstrumentContext& instrument() const noexcept;

        // Mutable access supports bootstrap through the existing AccountStore
        // API before a caller begins serialized request execution.
        [[nodiscard]] AccountStore& accounts();
        [[nodiscard]] const AccountStore& accounts() const noexcept;

        [[nodiscard]] const OrderReservationStore& reservations()
            const noexcept;
        [[nodiscard]] const OrderBook& order_book() const noexcept;
        [[nodiscard]] const Ledger& ledger() const noexcept;

    private:
        const InstrumentContext instrument_;
        AccountStore accounts_;
        OrderReservationStore reservations_;
        EventCollector events_;
        MatchingEngine matching_engine_;
        Ledger ledger_;
        ExecutionCoordinator execution_coordinator_;
        ExecutionSequencer sequencer_;
        std::unique_ptr<ExecutionWalWriter> wal_writer_;
        TradingRequestExecutor executor_;
        bool bootstrap_sealed_{};
    };
}  // namespace exchange
