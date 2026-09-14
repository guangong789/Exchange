#include "execution/contract_command_applier.hpp"

#include <exception>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    ContractCommandApplier::ContractCommandApplier(
        ContractStore& store,
        AccountStore& accounts,
        Ledger& ledger,
        AssetId quote_asset_id)
        : store_(store),
          accounts_(accounts),
          ledger_(ledger),
          quote_asset_id_(quote_asset_id) {
        if (quote_asset_id_ == 0) {
            throw std::invalid_argument(
                "Contract settlement quote AssetId must be non-zero");
        }
    }

    ContractResult ContractCommandApplier::validate_settlement(
        const SettlePaymentObligationExecutionCommand& command) const {
        const ContractResult domain = store_.validate_settlement(
            command.contract_id,
            command.acting_agent);
        if (domain != ContractResult::Success) {
            return domain;
        }

        const std::optional<Contract> contract = store_.find(
            command.contract_id);
        if (!contract.has_value()) {
            throw std::logic_error(
                "validated Contract settlement has no Contract");
        }

        switch (accounts_.validate_available_transfer(
                    command.payer_account_id,
                    command.payee_account_id,
                    quote_asset_id_,
                    contract->payment_obligation.quote_amount)) {
            case AvailableTransferValidationResult::Ready:
                return ContractResult::Success;
            case AvailableTransferValidationResult::AccountNotFound:
                return ContractResult::AccountNotFound;
            case AvailableTransferValidationResult::InsufficientFunds:
                return ContractResult::InsufficientFunds;
            case AvailableTransferValidationResult::DestinationOverflow:
                return ContractResult::BalanceOverflow;
        }
        throw std::logic_error(
            "unknown available transfer validation result");
    }

    ContractResult ContractCommandApplier::apply_settlement(
        const SettlePaymentObligationExecutionCommand& command) {
        const ContractResult validation = validate_settlement(command);
        if (validation != ContractResult::Success) {
            return validation;
        }

        const Contract contract = *store_.find(command.contract_id);
        auto prepared_ledger = ledger_.prepare_batch({
            make_contract_settlement_ledger_transaction(
                command.contract_id,
                command.payer_account_id,
                command.payee_account_id,
                quote_asset_id_,
                contract.payment_obligation.quote_amount),
        });

        if (accounts_.transfer_available(
                command.payer_account_id,
                command.payee_account_id,
                quote_asset_id_,
                contract.payment_obligation.quote_amount)
            != AvailableTransferResult::Success) {
            throw std::logic_error(
                "validated Contract settlement transfer was rejected");
        }

        try {
            const ContractResult settled = store_.mark_settled(
                command.contract_id,
                command.acting_agent);
            if (settled != ContractResult::Success) {
                throw std::logic_error(
                    "validated Contract settlement transition was rejected");
            }
        } catch (...) {
            const std::exception_ptr failure = std::current_exception();
            if (accounts_.transfer_available(
                    command.payee_account_id,
                    command.payer_account_id,
                    quote_asset_id_,
                    contract.payment_obligation.quote_amount)
                != AvailableTransferResult::Success) {
                throw std::logic_error(
                    "Contract settlement rollback was rejected");
            }
            std::rethrow_exception(failure);
        }

        prepared_ledger.commit();
        return ContractResult::Success;
    }

    ContractResult ContractCommandApplier::apply(
        const ExecutionCommand& command) {
        return std::visit(
            [this](const auto& payload) -> ContractResult {
                using Command = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<
                                  Command,
                                  CreateContractExecutionCommand>) {
                    return store_.create_contract(
                        payload.contract_id,
                        payload.proposer,
                        payload.counterparty,
                        payload.terms);
                } else if constexpr (std::is_same_v<
                                         Command,
                                         AcceptContractExecutionCommand>) {
                    return store_.accept_contract(
                        payload.contract_id,
                        payload.acting_agent);
                } else if constexpr (std::is_same_v<
                                         Command,
                                         RejectContractExecutionCommand>) {
                    return store_.reject_contract(
                        payload.contract_id,
                        payload.acting_agent);
                } else if constexpr (std::is_same_v<
                                         Command,
                                         FulfillResourceObligationExecutionCommand>) {
                    return store_.mark_fulfilled(
                        payload.contract_id,
                        payload.acting_agent);
                } else if constexpr (std::is_same_v<
                                         Command,
                                         SettlePaymentObligationExecutionCommand>) {
                    return apply_settlement(payload);
                } else {
                    throw std::logic_error(
                        "trading command passed to contract applier");
                }
            },
            command);
    }
}  // namespace exchange
