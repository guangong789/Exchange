#pragma once

#include "exchange/accounting/account.hpp"

#include <map>
#include <optional>

#include "exchange/agent/agent.hpp"

namespace exchange {
    class AgentRegistry {
    public:
        AgentRegistry() = default;
        AgentRegistry(const AgentRegistry&) = delete;
        AgentRegistry& operator=(const AgentRegistry&) = delete;
        AgentRegistry(AgentRegistry&&) = delete;
        AgentRegistry& operator=(AgentRegistry&&) = delete;

        [[nodiscard]] bool register_agent(AgentIdentity identity);
        [[nodiscard]] std::optional<AgentIdentity> find(
            AgentId agent_id) const;

    private:
        std::map<AgentId, AgentIdentity> agents_;
        std::map<AccountId, AgentId> account_owners_;
    };
}  // namespace exchange
