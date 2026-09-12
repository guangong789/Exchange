#include "durability/execution_recovery.hpp"

#include <exception>
#include <limits>
#include <map>
#include <utility>
#include <variant>

namespace exchange {
    ExecutionRecoveryException::ExecutionRecoveryException(
        ExecutionRecoveryFailure failure,
        WalSequence wal_sequence,
        std::string message)
        : std::runtime_error(std::move(message)),
          failure_(failure),
          wal_sequence_(wal_sequence) {}

    ExecutionRecoveryFailure
    ExecutionRecoveryException::failure() const noexcept {
        return failure_;
    }

    WalSequence ExecutionRecoveryException::wal_sequence() const noexcept {
        return wal_sequence_;
    }

    ExecutionRecovery::ExecutionRecovery(
        InstrumentContext instrument,
        AccountStore& accounts,
        OrderReservationStore& reservations,
        MatchingEngine& matching_engine,
        EventCollector& events,
        Ledger& ledger,
        ExecutionSequencer& sequencer,
        ExecutionCommandApplier& command_applier) noexcept
        : instrument_(instrument),
          accounts_(accounts),
          reservations_(reservations),
          matching_engine_(matching_engine),
          events_(events),
          ledger_(ledger),
          sequencer_(sequencer),
          command_applier_(command_applier) {}

    ExecutionRecoverySummary ExecutionRecovery::recover(
        std::span<const WalRecord> records,
        std::size_t writer_record_count,
        WalSequence writer_next_sequence) {
        verify_fresh_state();
        const AccountStore::AccountBalances bootstrap_accounts =
            accounts_.entries();
        if (writer_record_count != records.size()) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::WalPrefixMismatch,
                0,
                "WAL writer record count differs from recovery prefix"};
        }

        const WalSequence expected_writer_next = records.empty()
            ? 1
            : records.back().sequence
                  == std::numeric_limits<WalSequence>::max()
                ? records.back().sequence
                : records.back().sequence + 1;
        if (writer_next_sequence != expected_writer_next) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::WalPrefixMismatch,
                records.empty() ? 0 : records.back().sequence,
                "WAL writer next sequence differs from recovery prefix"};
        }

        std::size_t submit_attempts = 0;
        WalSequence expected_wal_sequence = 1;
        for (const WalRecord& record : records) {
            if (record.sequence != expected_wal_sequence) {
                throw ExecutionRecoveryException{
                    ExecutionRecoveryFailure::WalPrefixMismatch,
                    record.sequence,
                    "recovery WAL sequence is not contiguous"};
            }

            if (const auto* submit = std::get_if<SubmitExecutionCommand>(
                    &record.command)) {
                if (!sequencer_.advance_recovered_identity(
                        AssignedOrderIdentity{
                            submit->order.id,
                            submit->order.timestamp})) {
                    throw ExecutionRecoveryException{
                        ExecutionRecoveryFailure::ExecutionIdentityMismatch,
                        record.sequence,
                        "recorded execution identity does not match sequencer"};
                }
                ++submit_attempts;
            }

            try {
                static_cast<void>(command_applier_.apply(record.command));
            } catch (...) {
                events_.clear();
                std::throw_with_nested(ExecutionRecoveryException{
                    ExecutionRecoveryFailure::CommandApplication,
                    record.sequence,
                    "execution command failed during recovery"});
            }
            events_.clear();

            if (expected_wal_sequence
                != std::numeric_limits<WalSequence>::max()) {
                ++expected_wal_sequence;
            }
        }

        verify_recovered_state(
            records.empty() ? 0 : records.back().sequence,
            bootstrap_accounts);
        return ExecutionRecoverySummary{
            records.size(),
            submit_attempts,
            sequencer_.next_identity()};
    }

    void ExecutionRecovery::verify_fresh_state() const {
        for (const auto& [account_id, balances] : accounts_.entries()) {
            static_cast<void>(account_id);
            for (const auto& [asset_id, balance] : balances) {
                static_cast<void>(asset_id);
                if (balance.available < 0 || balance.reserved < 0) {
                    throw ExecutionRecoveryException{
                        ExecutionRecoveryFailure::StateNotFresh,
                        0,
                        "bootstrap contains a negative balance"};
                }
            }
        }
        if (matching_engine_.order_book().order_count() != 0
            || !reservations_.entries().empty()
            || !ledger_.entries().empty()
            || !events_.events().empty()
            || sequencer_.next_identity()
                != AssignedOrderIdentity{1, 1}) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::StateNotFresh,
                0,
                "recovery requires fresh execution state"};
        }
    }

    void ExecutionRecovery::verify_recovered_state(
        WalSequence last_sequence,
        const AccountStore::AccountBalances& bootstrap_accounts) const {
        const auto& ledger_entries = ledger_.entries();
        for (std::size_t index = 0; index < ledger_entries.size(); ++index) {
            if (ledger_entries[index].sequence != index + 1) {
                throw ExecutionRecoveryException{
                    ExecutionRecoveryFailure::LedgerInvariant,
                    last_sequence,
                    "recovered ledger sequence is not contiguous"};
            }
        }

        const auto& reservation_entries = reservations_.entries();
        if (matching_engine_.order_book().order_count()
            != reservation_entries.size()) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::ReservationInvariant,
                last_sequence,
                "live order and reservation counts differ"};
        }

        std::map<std::pair<AccountId, AssetId>, Amount> reserved_totals;
        for (const auto& [order_id, reservation] : reservation_entries) {
            const auto order = matching_engine_.order_book().find_order(
                order_id);
            if (!order.has_value()
                || !accounts_.contains_account(reservation.account_id)
                || reservation.original_amount <= 0
                || reservation.remaining_amount <= 0
                || reservation.remaining_amount
                    > reservation.original_amount) {
                throw ExecutionRecoveryException{
                    ExecutionRecoveryFailure::ReservationInvariant,
                    last_sequence,
                    "recovered reservation has no valid order or owner"};
            }

            ReservationRequirement minimum;
            try {
                minimum = calculate_order_reservation(
                    instrument_,
                    order->side,
                    order->price,
                    order->quantity);
            } catch (...) {
                std::throw_with_nested(ExecutionRecoveryException{
                    ExecutionRecoveryFailure::ReservationInvariant,
                    last_sequence,
                    "recovered live order has invalid financial values"});
            }
            if (reservation.asset_id != minimum.asset_id
                || reservation.remaining_amount < minimum.amount) {
                throw ExecutionRecoveryException{
                    ExecutionRecoveryFailure::ReservationInvariant,
                    last_sequence,
                    "recovered reservation does not cover its live order"};
            }

            Amount& total = reserved_totals[
                {reservation.account_id, reservation.asset_id}];
            if (reservation.remaining_amount
                > std::numeric_limits<Amount>::max() - total) {
                throw ExecutionRecoveryException{
                    ExecutionRecoveryFailure::ReservationInvariant,
                    last_sequence,
                    "recovered reservation total overflows"};
            }
            total += reservation.remaining_amount;
        }

        for (const auto& [account_id, balances] : accounts_.entries()) {
            for (const auto& [asset_id, balance] : balances) {
                const auto expected = reserved_totals.find(
                    {account_id, asset_id});
                const Amount order_reserved =
                    expected == reserved_totals.end() ? 0 : expected->second;
                Amount bootstrap_reserved = 0;
                const auto bootstrap_account = bootstrap_accounts.find(
                    account_id);
                if (bootstrap_account != bootstrap_accounts.end()) {
                    const auto bootstrap_balance =
                        bootstrap_account->second.find(asset_id);
                    if (bootstrap_balance
                        != bootstrap_account->second.end()) {
                        bootstrap_reserved =
                            bootstrap_balance->second.reserved;
                    }
                }
                if (order_reserved
                    > std::numeric_limits<Amount>::max()
                          - bootstrap_reserved) {
                    throw ExecutionRecoveryException{
                        ExecutionRecoveryFailure::ReservationInvariant,
                        last_sequence,
                        "recovered reserved balance total overflows"};
                }
                const Amount expected_reserved =
                    bootstrap_reserved + order_reserved;
                if (balance.available < 0 || balance.reserved < 0
                    || balance.reserved != expected_reserved) {
                    throw ExecutionRecoveryException{
                        ExecutionRecoveryFailure::ReservationInvariant,
                        last_sequence,
                        "recovered balance differs from reservations"};
                }
            }
        }

        if (!events_.events().empty()) {
            throw ExecutionRecoveryException{
                ExecutionRecoveryFailure::StaleEvents,
                last_sequence,
                "recovery left stale matching events"};
        }
    }
}  // namespace exchange
