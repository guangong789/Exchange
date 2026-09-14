#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

#include "accounting/account.hpp"
#include "agent/domain/agent_id.hpp"
#include "agent/domain/agent_identity.hpp"
#include "agent/domain/contract.hpp"
#include "agent/domain/economic_profile.hpp"
#include "agent/domain/preference_profile.hpp"
#include "agent/domain/world_state.hpp"
#include "core/types.hpp"

namespace exchange {
    struct AssetTargetObjective {
        AssetId asset_id{};
        Amount target_amount{};

        bool operator==(const AssetTargetObjective&) const = default;
    };

    struct ObjectiveProgress {
        AgentId agent_id{};
        AssetId asset_id{};
        Amount current_amount{};
        Amount target_amount{};
        bool achieved{};

        bool operator==(const ObjectiveProgress&) const = default;
    };

    struct ObservedOrder {
        OrderId order_id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};

        bool operator==(const ObservedOrder&) const = default;
    };

    struct AgentObservation {
        AgentId agent_id{};
        AccountId account_id{};
        WorldState world;
        std::optional<Balance> base_balance;
        std::optional<Balance> quote_balance;
        std::vector<ObservedOrder> active_orders;
        std::optional<ObjectiveProgress> objective;
        AgentEconomicProfile economic_profile;
        std::optional<AgentPreferenceProfile> preference_profile;
        std::vector<Contract> contracts;

        bool operator==(const AgentObservation&) const = default;
    };

    struct SubmitOrderAction {
        Side side{Side::Buy};
        Price price{};
        Quantity quantity{};

        bool operator==(const SubmitOrderAction&) const = default;
    };

    struct CancelOrderAction {
        OrderId order_id{};

        bool operator==(const CancelOrderAction&) const = default;
    };

    struct HoldAction {
        bool operator==(const HoldAction&) const = default;
    };

    struct ProposeContractAction {
        AgentId counterparty{};
        ContractTerms terms;

        bool operator==(const ProposeContractAction&) const = default;
    };

    struct AcceptContractAction {
        ContractId contract_id{};

        bool operator==(const AcceptContractAction&) const = default;
    };

    struct RejectContractAction {
        ContractId contract_id{};

        bool operator==(const RejectContractAction&) const = default;
    };

    struct FulfillResourceObligationAction {
        ContractId contract_id{};

        bool operator==(
            const FulfillResourceObligationAction&) const = default;
    };

    struct SettlePaymentObligationAction {
        ContractId contract_id{};

        bool operator==(
            const SettlePaymentObligationAction&) const = default;
    };

    using AgentAction = std::variant<
        SubmitOrderAction,
        CancelOrderAction,
        ProposeContractAction,
        AcceptContractAction,
        RejectContractAction,
        FulfillResourceObligationAction,
        SettlePaymentObligationAction,
        HoldAction>;

    enum class AgentSubmitStatus {
        Accepted,
        AccountNotFound,
        InsufficientFunds,
        DuplicateOrder,
        InvalidOrder,
        CounterpartyNotAccountBacked,
    };

    enum class AgentCancelStatus {
        Cancelled,
        AccountNotFound,
        NotFound,
        NotOwner,
    };

    struct SubmitActionResult {
        OrderId order_id{};
        Timestamp timestamp{};
        AgentSubmitStatus status{AgentSubmitStatus::InvalidOrder};

        bool operator==(const SubmitActionResult&) const = default;
    };

    struct CancelActionResult {
        AgentCancelStatus status{AgentCancelStatus::NotFound};

        bool operator==(const CancelActionResult&) const = default;
    };

    struct HoldActionResult {
        bool operator==(const HoldActionResult&) const = default;
    };

    struct ContractActionResult {
        std::optional<ContractId> contract_id;
        ContractResult status{ContractResult::InvalidTerms};

        bool operator==(const ContractActionResult&) const = default;
    };

    using AgentActionResult = std::variant<
        SubmitActionResult,
        CancelActionResult,
        ContractActionResult,
        HoldActionResult>;

    class AgentDecisionError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class AgentDecisionProvider {
    public:
        virtual ~AgentDecisionProvider() = default;

        [[nodiscard]] virtual AgentAction decide(
            const AgentObservation& observation) const = 0;
    };

}  // namespace exchange
