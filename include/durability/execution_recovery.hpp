#pragma once

#include "accounting/account_store.hpp"
#include "accounting/financial_conversion.hpp"
#include "accounting/ledger.hpp"
#include "accounting/order_reservation_store.hpp"
#include "durability/execution_wal.hpp"
#include "execution/contract_command_applier.hpp"
#include "execution/contract_sequencer.hpp"
#include "execution/trading_command_applier.hpp"
#include "execution/execution_sequencer.hpp"
#include "matching/event_collector.hpp"
#include "matching/matching_engine.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {
    enum class ExecutionRecoveryFailure {
        StateNotFresh,
        WalPrefixMismatch,
        ExecutionIdentityMismatch,
        ContractIdentityMismatch,
        CommandApplication,
        ContractInvariant,
        LedgerInvariant,
        ReservationInvariant,
        StaleEvents,
    };

    class ExecutionRecoveryException : public std::runtime_error {
    public:
        ExecutionRecoveryException(
            ExecutionRecoveryFailure failure,
            WalSequence wal_sequence,
            std::string message);

        [[nodiscard]] ExecutionRecoveryFailure failure() const noexcept;
        [[nodiscard]] WalSequence wal_sequence() const noexcept;

    private:
        ExecutionRecoveryFailure failure_;
        WalSequence wal_sequence_;
    };

    struct ExecutionRecoverySummary {
        std::size_t records_replayed{};
        std::size_t submit_attempts{};
        AssignedOrderIdentity next_execution_identity;
        ContractId next_contract_id{};
    };

    struct RecoveredTradingOutcome {
        WalSequence wal_sequence{};
        TradingResult result{TradingResult::InvalidRequest};

        bool operator==(const RecoveredTradingOutcome&) const = default;
    };

    class ExecutionRecovery {
    public:
        ExecutionRecovery(
            InstrumentContext instrument,
            AccountStore& accounts,
            OrderReservationStore& reservations,
            MatchingEngine& matching_engine,
            EventCollector& events,
            Ledger& ledger,
            ExecutionSequencer& sequencer,
            TradingCommandApplier& command_applier,
            ContractStore& contracts,
            ContractSequencer& contract_sequencer,
            ContractCommandApplier& contract_command_applier) noexcept;

        // Optional outcomes replace the caller's collection only after all
        // recovery checks succeed; on failure the collection is unchanged.
        [[nodiscard]] ExecutionRecoverySummary recover(
            std::span<const WalRecord> records,
            std::size_t writer_record_count,
            WalSequence writer_next_sequence,
            std::vector<RecoveredTradingOutcome>* recovered_trading_outcomes
                = nullptr);

    private:
        void verify_fresh_state() const;
        void verify_recovered_state(
            WalSequence last_sequence,
            const AccountStore::AccountBalances& bootstrap_accounts) const;

        const InstrumentContext instrument_;
        AccountStore& accounts_;
        OrderReservationStore& reservations_;
        MatchingEngine& matching_engine_;
        EventCollector& events_;
        Ledger& ledger_;
        ExecutionSequencer& sequencer_;
        TradingCommandApplier& command_applier_;
        ContractStore& contracts_;
        ContractSequencer& contract_sequencer_;
        ContractCommandApplier& contract_command_applier_;
    };
}  // namespace exchange
