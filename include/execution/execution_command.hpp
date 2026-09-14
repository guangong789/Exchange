#pragma once

#include "accounting/account.hpp"
#include "agent/domain/contract.hpp"
#include "core/types.hpp"
#include "matching/order.hpp"

#include <variant>

namespace exchange {
    struct SubmitExecutionCommand {
        RequestId request_id{};
        AccountId account_id{};
        Order order;
    };

    struct CancelExecutionCommand {
        RequestId request_id{};
        AccountId account_id{};
        OrderId order_id{};
    };

    struct CreateContractExecutionCommand {
        ContractId contract_id{};
        AgentId proposer{};
        AgentId counterparty{};
        ContractTerms terms;

        bool operator==(const CreateContractExecutionCommand&) const = default;
    };

    struct AcceptContractExecutionCommand {
        ContractId contract_id{};
        AgentId acting_agent{};

        bool operator==(const AcceptContractExecutionCommand&) const = default;
    };

    struct RejectContractExecutionCommand {
        ContractId contract_id{};
        AgentId acting_agent{};

        bool operator==(const RejectContractExecutionCommand&) const = default;
    };

    struct FulfillResourceObligationExecutionCommand {
        ContractId contract_id{};
        AgentId acting_agent{};

        bool operator==(
            const FulfillResourceObligationExecutionCommand&) const = default;
    };

    struct SettlePaymentObligationExecutionCommand {
        ContractId contract_id{};
        AgentId acting_agent{};
        AccountId payer_account_id{};
        AccountId payee_account_id{};

        bool operator==(
            const SettlePaymentObligationExecutionCommand&) const = default;
    };

    using ExecutionCommand = std::variant<
        SubmitExecutionCommand,
        CancelExecutionCommand,
        CreateContractExecutionCommand,
        AcceptContractExecutionCommand,
        RejectContractExecutionCommand,
        FulfillResourceObligationExecutionCommand,
        SettlePaymentObligationExecutionCommand>;
}  // namespace exchange
