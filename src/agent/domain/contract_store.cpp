#include "agent/domain/contract_store.hpp"

#include <limits>
#include <utility>

namespace exchange {
    namespace {
        bool valid_terms(
            AgentId proposer,
            AgentId counterparty,
            const ContractTerms& terms) noexcept {
            if (proposer == 0 || counterparty == 0
                || proposer == counterparty
                || terms.payer == 0 || terms.payee == 0
                || terms.payer == terms.payee
                || terms.quote_payment_amount <= 0
                || terms.resource_quantity <= 0
                || terms.resource != ResourceKind::ComputeCredit) {
                return false;
            }
            return (terms.payer == proposer
                    && terms.payee == counterparty)
                || (terms.payer == counterparty
                    && terms.payee == proposer);
        }

        bool flags_match_state(const Contract& contract) noexcept {
            switch (contract.state) {
                case ContractState::Proposed:
                case ContractState::Accepted:
                case ContractState::Rejected:
                    return !contract.payment_obligation.fulfilled
                        && !contract.resource_delivery_obligation.fulfilled;
                case ContractState::Fulfilled:
                    return !contract.payment_obligation.fulfilled
                        && contract.resource_delivery_obligation.fulfilled;
                case ContractState::Settled:
                    return contract.payment_obligation.fulfilled
                        && contract.resource_delivery_obligation.fulfilled;
            }
            return false;
        }
    }  // namespace

    ContractResult ContractStore::validate_creation(
        ContractId contract_id,
        AgentId proposer,
        AgentId counterparty,
        const ContractTerms& terms) const {
        if (contract_id == 0
            || contract_id == std::numeric_limits<ContractId>::max()
            || !valid_terms(proposer, counterparty, terms)) {
            return ContractResult::InvalidTerms;
        }
        return contracts_.contains(contract_id)
            ? ContractResult::InvalidTransition
            : ContractResult::Success;
    }

    ContractResult ContractStore::create_contract(
        ContractId contract_id,
        AgentId proposer,
        AgentId counterparty,
        ContractTerms terms) {
        const ContractResult validation = validate_creation(
            contract_id,
            proposer,
            counterparty,
            terms);
        if (validation != ContractResult::Success) {
            return validation;
        }

        contracts_.emplace(
            contract_id,
            Contract{
                contract_id,
                proposer,
                counterparty,
                terms,
                PaymentObligation{
                    terms.payer,
                    terms.payee,
                    terms.quote_payment_amount,
                    false},
                ResourceDeliveryObligation{
                    terms.payee,
                    terms.payer,
                    terms.resource,
                    terms.resource_quantity,
                    false},
                ContractState::Proposed});
        return ContractResult::Success;
    }

    std::optional<Contract> ContractStore::find(
        ContractId contract_id) const {
        const auto contract = contracts_.find(contract_id);
        if (contract == contracts_.end()) {
            return std::nullopt;
        }
        return contract->second;
    }

    std::vector<Contract> ContractStore::find_relevant(
        AgentId agent_id) const {
        std::vector<Contract> relevant;
        for (const auto& [contract_id, contract] : contracts_) {
            static_cast<void>(contract_id);
            if (contract.proposer == agent_id
                || contract.counterparty == agent_id
                || contract.terms.payer == agent_id
                || contract.terms.payee == agent_id) {
                relevant.push_back(contract);
            }
        }
        return relevant;
    }

    ContractResult ContractStore::validate_accept(
        ContractId contract_id,
        AgentId acting_agent) const {
        const auto contract = contracts_.find(contract_id);
        if (contract == contracts_.end()) {
            return ContractResult::ContractNotFound;
        }
        if (contract->second.state != ContractState::Proposed) {
            return ContractResult::InvalidTransition;
        }
        if (acting_agent != contract->second.counterparty) {
            return ContractResult::UnauthorizedActor;
        }
        return ContractResult::Success;
    }

    ContractResult ContractStore::accept_contract(
        ContractId contract_id,
        AgentId acting_agent) {
        const ContractResult validation = validate_accept(
            contract_id,
            acting_agent);
        if (validation != ContractResult::Success) {
            return validation;
        }
        auto contract = contracts_.find(contract_id);
        contract->second.state = ContractState::Accepted;
        return ContractResult::Success;
    }

    ContractResult ContractStore::validate_reject(
        ContractId contract_id,
        AgentId acting_agent) const {
        const auto contract = contracts_.find(contract_id);
        if (contract == contracts_.end()) {
            return ContractResult::ContractNotFound;
        }
        if (contract->second.state != ContractState::Proposed) {
            return ContractResult::InvalidTransition;
        }
        if (acting_agent != contract->second.counterparty) {
            return ContractResult::UnauthorizedActor;
        }
        return ContractResult::Success;
    }

    ContractResult ContractStore::reject_contract(
        ContractId contract_id,
        AgentId acting_agent) {
        const ContractResult validation = validate_reject(
            contract_id,
            acting_agent);
        if (validation != ContractResult::Success) {
            return validation;
        }
        auto contract = contracts_.find(contract_id);
        contract->second.state = ContractState::Rejected;
        return ContractResult::Success;
    }

    ContractResult ContractStore::validate_fulfillment(
        ContractId contract_id,
        AgentId acting_agent) const {
        const auto contract = contracts_.find(contract_id);
        if (contract == contracts_.end()) {
            return ContractResult::ContractNotFound;
        }
        if (contract->second.state != ContractState::Accepted) {
            return ContractResult::InvalidTransition;
        }
        if (acting_agent
            != contract->second.resource_delivery_obligation.debtor) {
            return ContractResult::UnauthorizedActor;
        }
        return ContractResult::Success;
    }

    ContractResult ContractStore::mark_fulfilled(
        ContractId contract_id,
        AgentId acting_agent) {
        const ContractResult validation = validate_fulfillment(
            contract_id,
            acting_agent);
        if (validation != ContractResult::Success) {
            return validation;
        }
        auto contract = contracts_.find(contract_id);
        contract->second.resource_delivery_obligation.fulfilled = true;
        contract->second.state = ContractState::Fulfilled;
        return ContractResult::Success;
    }

    ContractResult ContractStore::validate_settlement(
        ContractId contract_id,
        AgentId acting_agent) const {
        const auto contract = contracts_.find(contract_id);
        if (contract == contracts_.end()) {
            return ContractResult::ContractNotFound;
        }
        if (contract->second.state != ContractState::Fulfilled) {
            return ContractResult::InvalidTransition;
        }
        if (acting_agent != contract->second.payment_obligation.debtor) {
            return ContractResult::UnauthorizedActor;
        }
        return ContractResult::Success;
    }

    ContractResult ContractStore::mark_settled(
        ContractId contract_id,
        AgentId acting_agent) {
        const ContractResult validation = validate_settlement(
            contract_id,
            acting_agent);
        if (validation != ContractResult::Success) {
            return validation;
        }
        auto contract = contracts_.find(contract_id);
        contract->second.payment_obligation.fulfilled = true;
        contract->second.state = ContractState::Settled;
        return ContractResult::Success;
    }

    std::size_t ContractStore::size() const noexcept {
        return contracts_.size();
    }

    bool ContractStore::invariants_hold() const noexcept {
        ContractId expected_id = 1;
        for (const auto& [id, contract] : contracts_) {
            if (id != expected_id || contract.id != id
                || !valid_terms(
                    contract.proposer,
                    contract.counterparty,
                    contract.terms)
                || contract.payment_obligation
                    != PaymentObligation{
                        contract.terms.payer,
                        contract.terms.payee,
                        contract.terms.quote_payment_amount,
                        contract.payment_obligation.fulfilled}
                || contract.resource_delivery_obligation
                    != ResourceDeliveryObligation{
                        contract.terms.payee,
                        contract.terms.payer,
                        contract.terms.resource,
                        contract.terms.resource_quantity,
                        contract.resource_delivery_obligation.fulfilled}
                || !flags_match_state(contract)) {
                return false;
            }
            if (expected_id == std::numeric_limits<ContractId>::max()) {
                return false;
            }
            ++expected_id;
        }
        return true;
    }
}  // namespace exchange
