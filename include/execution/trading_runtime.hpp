#pragma once

#include "accounting/account_store.hpp"
#include "accounting/execution_coordinator.hpp"
#include "accounting/financial_conversion.hpp"
#include "accounting/ledger.hpp"
#include "accounting/order_reservation_store.hpp"
#include "agent/domain/agent_registry.hpp"
#include "agent/domain/contract_store.hpp"
#include "durability/execution_wal_writer.hpp"
#include "execution/contract_command_applier.hpp"
#include "execution/contract_request_executor.hpp"
#include "execution/contract_sequencer.hpp"
#include "execution/execution_runtime_status.hpp"
#include "execution/execution_sequencer.hpp"
#include "execution/trading_bootstrap.hpp"
#include "execution/trading_request_executor.hpp"
#include "matching/event_collector.hpp"
#include "matching/matching_engine.hpp"
#include "matching/order_book.hpp"

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
        [[nodiscard]] ContractRequestExecutor& contract_executor() noexcept;

        [[nodiscard]] const InstrumentContext& instrument() const noexcept;

        // Mutable access supports bootstrap through the existing AccountStore
        // API before a caller begins serialized request execution.
        [[nodiscard]] AccountStore& accounts();
        [[nodiscard]] const AccountStore& accounts() const noexcept;

        [[nodiscard]] const OrderReservationStore& reservations()
            const noexcept;
        [[nodiscard]] const OrderBook& order_book() const noexcept;
        [[nodiscard]] const Ledger& ledger() const noexcept;
        [[nodiscard]] AgentRegistry& agent_registry() noexcept;
        [[nodiscard]] const AgentRegistry& agent_registry() const noexcept;
        [[nodiscard]] const ContractStore& contracts() const noexcept;

    private:
        const InstrumentContext instrument_;
        AccountStore accounts_;
        OrderReservationStore reservations_;
        EventCollector events_;
        MatchingEngine matching_engine_;
        Ledger ledger_;
        ExecutionCoordinator execution_coordinator_;
        ExecutionSequencer sequencer_;
        AgentRegistry agent_registry_;
        ContractStore contracts_;
        ContractSequencer contract_sequencer_;
        ContractCommandApplier contract_command_applier_;
        ExecutionRuntimeStatus runtime_status_;
        std::unique_ptr<ExecutionWalWriter> wal_writer_;
        TradingRequestExecutor executor_;
        ContractRequestExecutor contract_executor_;
        bool bootstrap_sealed_{};
    };
}  // namespace exchange
