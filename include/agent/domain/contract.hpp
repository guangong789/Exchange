#pragma once

#include <cstdint>

#include "agent/domain/obligation.hpp"

namespace exchange {
    using ContractId = std::uint64_t;

    struct ContractTerms {
        AgentId payer{};
        AgentId payee{};
        Amount quote_payment_amount{};
        ResourceKind resource{ResourceKind::ComputeCredit};
        ResourceQuantity resource_quantity{};

        bool operator==(const ContractTerms&) const = default;
    };

    enum class ContractState : std::uint8_t {
        Proposed,
        Accepted,
        Rejected,
        // The complete resource-delivery obligation is fulfilled; the
        // payment obligation remains outstanding.
        Fulfilled,
        Settled,
    };

    enum class ContractResult {
        Success,
        ContractNotFound,
        InvalidTerms,
        InvalidTransition,
        UnauthorizedActor,
        AccountNotFound,
        InsufficientFunds,
        BalanceOverflow,
        ContractIdExhausted,
    };

    struct Contract {
        ContractId id{};
        AgentId proposer{};
        AgentId counterparty{};
        ContractTerms terms;
        PaymentObligation payment_obligation;
        ResourceDeliveryObligation resource_delivery_obligation;
        ContractState state{ContractState::Proposed};

        bool operator==(const Contract&) const = default;
    };
}  // namespace exchange
