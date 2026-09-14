#include "execution/contract_request_executor.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace exchange {
    ContractRequestExecutor::ContractRequestExecutor(
        const AgentRegistry& registry,
        ContractStore& store,
        ContractSequencer& sequencer,
        ContractCommandApplier& command_applier,
        ExecutionCommandJournal* command_journal,
        ExecutionRuntimeStatus* runtime_status) noexcept
        : registry_(registry),
          store_(store),
          sequencer_(sequencer),
          command_applier_(command_applier),
          command_journal_(command_journal),
          runtime_status_(runtime_status) {}

    ContractExecutionResponse ContractRequestExecutor::create_contract(
        const CreateContractRequest& request) {
        ensure_usable();
        if (request.proposer == 0 || request.counterparty == 0
            || !registry_.find(request.proposer).has_value()
            || !registry_.find(request.counterparty).has_value()) {
            return {ContractResult::InvalidTerms, std::nullopt};
        }
        if (sequencer_.next_id()
            == std::numeric_limits<ContractId>::max()) {
            return {ContractResult::ContractIdExhausted, std::nullopt};
        }
        const ContractResult admission = store_.validate_creation(
            sequencer_.next_id(),
            request.proposer,
            request.counterparty,
            request.terms);
        if (admission != ContractResult::Success) {
            return {admission, std::nullopt};
        }

        const ContractId contract_id = sequencer_.allocate();
        const ExecutionCommand command = CreateContractExecutionCommand{
            contract_id,
            request.proposer,
            request.counterparty,
            request.terms};
        const ContractResult result = execute_transition(
            ContractResult::Success,
            command);
        return {
            result,
            result == ContractResult::Success
                ? std::optional<ContractId>{contract_id}
                : std::nullopt};
    }

    ContractResult ContractRequestExecutor::accept_contract(
        ContractId contract_id,
        AgentId acting_agent) {
        return execute_transition(
            store_.validate_accept(contract_id, acting_agent),
            AcceptContractExecutionCommand{contract_id, acting_agent});
    }

    ContractResult ContractRequestExecutor::reject_contract(
        ContractId contract_id,
        AgentId acting_agent) {
        return execute_transition(
            store_.validate_reject(contract_id, acting_agent),
            RejectContractExecutionCommand{contract_id, acting_agent});
    }

    ContractResult ContractRequestExecutor::fulfill_resource(
        ContractId contract_id,
        AgentId acting_agent) {
        return execute_transition(
            store_.validate_fulfillment(contract_id, acting_agent),
            FulfillResourceObligationExecutionCommand{
                contract_id,
                acting_agent});
    }

    ContractResult ContractRequestExecutor::settle_payment(
        ContractId contract_id,
        AgentId acting_agent) {
        ensure_usable();
        const ContractResult domain = store_.validate_settlement(
            contract_id,
            acting_agent);
        if (domain != ContractResult::Success) {
            return domain;
        }

        const std::optional<Contract> contract = store_.find(contract_id);
        if (!contract.has_value()) {
            throw std::logic_error(
                "validated Contract settlement has no Contract");
        }
        const std::optional<AgentIdentity> payer = registry_.find(
            contract->payment_obligation.debtor);
        const std::optional<AgentIdentity> payee = registry_.find(
            contract->payment_obligation.creditor);
        if (!payer.has_value() || !payee.has_value()) {
            return ContractResult::AccountNotFound;
        }

        const SettlePaymentObligationExecutionCommand command{
            contract_id,
            acting_agent,
            payer->account_id,
            payee->account_id};
        return execute_transition(
            command_applier_.validate_settlement(command),
            command);
    }

    bool ContractRequestExecutor::poisoned() const noexcept {
        return poisoned_
            || (runtime_status_ != nullptr && runtime_status_->poisoned);
    }

    bool ContractRequestExecutor::durable_processing_started()
        const noexcept {
        return durable_processing_started_
            || (runtime_status_ != nullptr
                && runtime_status_->durable_processing_started);
    }

    void ContractRequestExecutor::attach_command_journal(
        ExecutionCommandJournal& command_journal) noexcept {
        command_journal_ = &command_journal;
    }

    ContractResult ContractRequestExecutor::execute_transition(
        ContractResult admission,
        ExecutionCommand command) {
        ensure_usable();
        if (admission != ContractResult::Success) {
            return admission;
        }
        if (command_journal_ != nullptr) {
            mark_durable_processing_started();
        }

        try {
            if (command_journal_ != nullptr) {
                command_journal_->append(command);
            }
            const ContractResult applied = command_applier_.apply(command);
            if (applied != ContractResult::Success) {
                throw std::logic_error(
                    "durable contract admission/apply result mismatch");
            }
            return applied;
        } catch (...) {
            poison();
            throw;
        }
    }

    void ContractRequestExecutor::ensure_usable() const {
        if (poisoned()) {
            throw std::logic_error("contract request executor is poisoned");
        }
    }

    void ContractRequestExecutor::mark_durable_processing_started()
        noexcept {
        durable_processing_started_ = true;
        if (runtime_status_ != nullptr) {
            runtime_status_->durable_processing_started = true;
        }
    }

    void ContractRequestExecutor::poison() noexcept {
        poisoned_ = true;
        if (runtime_status_ != nullptr) {
            runtime_status_->poisoned = true;
        }
    }
}  // namespace exchange
