#include "exchange/agent_registry.hpp"

#include <stdexcept>

namespace exchange {
    bool AgentRegistry::register_agent(AgentIdentity identity) {
        if (identity.agent_id == 0) {
            throw std::invalid_argument("Agent ID must be non-zero");
        }
        if (identity.account_id == 0) {
            throw std::invalid_argument("Agent Account ID must be non-zero");
        }
        if (agents_.contains(identity.agent_id)
            || account_owners_.contains(identity.account_id)) {
            return false;
        }

        const auto [agent, inserted] =
            agents_.emplace(identity.agent_id, identity);
        static_cast<void>(inserted);
        try {
            account_owners_.emplace(
                identity.account_id,
                identity.agent_id);
        } catch (...) {
            agents_.erase(agent);
            throw;
        }
        return true;
    }

    std::optional<AgentIdentity> AgentRegistry::find(
        AgentId agent_id) const {
        if (agent_id == 0) {
            throw std::invalid_argument("Agent ID must be non-zero");
        }

        const auto agent = agents_.find(agent_id);
        if (agent == agents_.end()) {
            return std::nullopt;
        }
        return agent->second;
    }
}  // namespace exchange
