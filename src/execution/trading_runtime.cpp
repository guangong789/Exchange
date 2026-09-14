#include "execution/trading_runtime.hpp"

#include "durability/execution_recovery.hpp"

#include <stdexcept>
#include <utility>

namespace exchange {
    TradingRuntime::TradingRuntime(InstrumentContext instrument)
        : instrument_(instrument),
          matching_engine_(events_),
          execution_coordinator_(
              instrument_,
              accounts_,
              reservations_,
              matching_engine_,
              events_,
              ledger_),
          contract_command_applier_(
              contracts_,
              accounts_,
              ledger_,
              instrument_.quote_asset),
          executor_(
              instrument_,
              execution_coordinator_,
              events_,
              sequencer_,
              nullptr,
              &runtime_status_),
          contract_executor_(
              agent_registry_,
              contracts_,
              contract_sequencer_,
              contract_command_applier_,
              nullptr,
              &runtime_status_) {}

    std::unique_ptr<TradingRuntime> TradingRuntime::create_durable(
        InstrumentContext instrument,
        std::string wal_path,
        const TradingBootstrapConfig& bootstrap) {
        auto runtime = std::unique_ptr<TradingRuntime>{
            new TradingRuntime{instrument}};
        const BootstrapFingerprint bootstrap_fingerprint =
            calculate_bootstrap_fingerprint(bootstrap);
        apply_trading_bootstrap(bootstrap, runtime->accounts_);

        runtime->wal_writer_ = std::make_unique<ExecutionWalWriter>(
            std::move(wal_path),
            runtime->instrument_,
            bootstrap_fingerprint);
        std::vector<WalRecord> records =
            runtime->wal_writer_->take_recovered_records();

        TradingCommandApplier recovery_applier{
            runtime->execution_coordinator_,
            runtime->events_};
        ExecutionRecovery recovery{
            runtime->instrument_,
            runtime->accounts_,
            runtime->reservations_,
            runtime->matching_engine_,
            runtime->events_,
            runtime->ledger_,
            runtime->sequencer_,
            recovery_applier,
            runtime->contracts_,
            runtime->contract_sequencer_,
            runtime->contract_command_applier_};
        static_cast<void>(recovery.recover(
            records,
            runtime->wal_writer_->record_count(),
            runtime->wal_writer_->next_sequence()));

        if (runtime->wal_writer_->poisoned()
            || runtime->executor_.poisoned()) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::StateNotFresh,
                records.empty() ? 0 : records.back().sequence,
                "recovered runtime is poisoned"};
        }

        runtime->executor_.attach_command_journal(*runtime->wal_writer_);
        runtime->contract_executor_.attach_command_journal(
            *runtime->wal_writer_);
        runtime->bootstrap_sealed_ = true;
        return runtime;
    }

    TradingRequestExecutor& TradingRuntime::executor() noexcept {
        return executor_;
    }

    ContractRequestExecutor& TradingRuntime::contract_executor() noexcept {
        return contract_executor_;
    }

    const InstrumentContext& TradingRuntime::instrument() const noexcept {
        return instrument_;
    }

    AccountStore& TradingRuntime::accounts() {
        if (bootstrap_sealed_ || executor_.durable_processing_started()) {
            throw std::logic_error(
                "account bootstrap after durable recovery is unsupported");
        }
        return accounts_;
    }

    const AccountStore& TradingRuntime::accounts() const noexcept {
        return accounts_;
    }

    const OrderReservationStore& TradingRuntime::reservations()
        const noexcept {
        return reservations_;
    }

    const OrderBook& TradingRuntime::order_book() const noexcept {
        return matching_engine_.order_book();
    }

    const Ledger& TradingRuntime::ledger() const noexcept {
        return ledger_;
    }

    AgentRegistry& TradingRuntime::agent_registry() noexcept {
        return agent_registry_;
    }

    const AgentRegistry& TradingRuntime::agent_registry() const noexcept {
        return agent_registry_;
    }

    const ContractStore& TradingRuntime::contracts() const noexcept {
        return contracts_;
    }
}  // namespace exchange
