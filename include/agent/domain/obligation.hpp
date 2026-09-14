#pragma once

#include <cstdint>

#include "accounting/account.hpp"
#include "agent/domain/agent_id.hpp"

namespace exchange {
    using ResourceQuantity = std::int64_t;

    enum class ResourceKind : std::uint8_t {
        ComputeCredit = 1,
    };

    struct PaymentObligation {
        AgentId debtor{};
        AgentId creditor{};
        Amount quote_amount{};
        bool fulfilled{};

        bool operator==(const PaymentObligation&) const = default;
    };

    struct ResourceDeliveryObligation {
        AgentId debtor{};
        AgentId creditor{};
        ResourceKind resource{ResourceKind::ComputeCredit};
        ResourceQuantity quantity{};
        bool fulfilled{};

        bool operator==(const ResourceDeliveryObligation&) const = default;
    };
}  // namespace exchange
