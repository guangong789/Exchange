#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "exchange/accounting/account.hpp"
#include "exchange/accounting/execution_coordinator.hpp"
#include "exchange/agent/external_market.hpp"
#include "exchange/core/types.hpp"

namespace exchange {
    using AgentId = std::uint64_t;

    struct AgentIdentity {
        AgentId agent_id{};
        AccountId account_id{};

        bool operator==(const AgentIdentity&) const = default;
    };

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

    struct AgentObservation {
        AgentId agent_id{};
        AccountId account_id{};
        std::optional<Price> best_bid;
        std::optional<Price> best_ask;
        std::optional<Balance> base_balance;
        std::optional<Balance> quote_balance;
        std::optional<ObjectiveProgress> objective;
        std::optional<ExternalMarketSnapshot> external_market;

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

    using AgentAction = std::variant<
        SubmitOrderAction,
        CancelOrderAction,
        HoldAction>;

    struct SubmitActionResult {
        OrderId order_id{};
        Timestamp timestamp{};
        SubmitResult result{SubmitResult::InvalidOrder};

        bool operator==(const SubmitActionResult&) const = default;
    };

    struct CancelActionResult {
        CancelResult result{CancelResult::NotFound};

        bool operator==(const CancelActionResult&) const = default;
    };

    struct HoldActionResult {
        bool operator==(const HoldActionResult&) const = default;
    };

    using AgentActionResult = std::variant<
        SubmitActionResult,
        CancelActionResult,
        HoldActionResult>;

    class AgentPolicy {
    public:
        virtual ~AgentPolicy() = default;

        [[nodiscard]] virtual AgentAction decide(
            const AgentObservation& observation) const = 0;
    };

    class ThresholdBuyPolicy final : public AgentPolicy {
    public:
        ThresholdBuyPolicy(Price maximum_price, Quantity quantity);

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        Price maximum_price_;
        Quantity quantity_;
    };

    class AcquireAssetPolicy final : public AgentPolicy {
    public:
        explicit AcquireAssetPolicy(Quantity quantity);

        [[nodiscard]] AgentAction decide(
            const AgentObservation& observation) const override;

    private:
        Quantity quantity_;
    };
}  // namespace exchange
