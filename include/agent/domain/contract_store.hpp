#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "agent/domain/contract.hpp"

namespace exchange {
    class ContractStore {
    public:
        ContractStore() = default;
        ContractStore(const ContractStore&) = delete;
        ContractStore& operator=(const ContractStore&) = delete;
        ContractStore(ContractStore&&) = delete;
        ContractStore& operator=(ContractStore&&) = delete;

        [[nodiscard]] ContractResult validate_creation(
            ContractId contract_id,
            AgentId proposer,
            AgentId counterparty,
            const ContractTerms& terms) const;
        [[nodiscard]] ContractResult create_contract(
            ContractId contract_id,
            AgentId proposer,
            AgentId counterparty,
            ContractTerms terms);

        [[nodiscard]] std::optional<Contract> find(
            ContractId contract_id) const;
        [[nodiscard]] std::vector<Contract> find_relevant(
            AgentId agent_id) const;

        [[nodiscard]] ContractResult validate_accept(
            ContractId contract_id,
            AgentId acting_agent) const;
        [[nodiscard]] ContractResult accept_contract(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult validate_reject(
            ContractId contract_id,
            AgentId acting_agent) const;
        [[nodiscard]] ContractResult reject_contract(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult validate_fulfillment(
            ContractId contract_id,
            AgentId acting_agent) const;
        // The resource-delivery debtor records complete delivery of the
        // immutable resource and quantity. This performs no resource or
        // payment accounting; the payment obligation remains outstanding.
        [[nodiscard]] ContractResult mark_fulfilled(
            ContractId contract_id,
            AgentId acting_agent);
        [[nodiscard]] ContractResult validate_settlement(
            ContractId contract_id,
            AgentId acting_agent) const;
        // The payment debtor (the payer) acknowledges settlement. No balance
        // transfer is performed by this in-memory domain store.
        [[nodiscard]] ContractResult mark_settled(
            ContractId contract_id,
            AgentId acting_agent);

        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] bool invariants_hold() const noexcept;

    private:
        std::map<ContractId, Contract> contracts_;
    };
}  // namespace exchange
