#pragma once

#include "accounting/account.hpp"
#include "agent/domain/agent_id.hpp"

namespace exchange {
    struct AgentIdentity {
        AgentId agent_id{};
        AccountId account_id{};

        bool operator==(const AgentIdentity&) const = default;
    };
}  // namespace exchange
