#include "exchange/agent/agent_objective.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    ObjectiveEvaluator::ObjectiveEvaluator(
        const AgentRegistry& registry,
        const AccountStore& accounts) noexcept
        : registry_(registry), accounts_(accounts) {}

    ObjectiveProgress ObjectiveEvaluator::evaluate(
        AgentId agent_id,
        const AssetTargetObjective& objective) const {
        if (objective.asset_id == 0) {
            throw std::invalid_argument(
                "Objective Asset ID must be non-zero");
        }
        if (objective.target_amount <= 0) {
            throw std::invalid_argument(
                "Objective target amount must be positive");
        }

        const auto identity = registry_.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("Objective Agent does not exist");
        }
        if (!accounts_.contains_account(identity->account_id)) {
            throw std::logic_error(
                "Objective Agent Account does not exist");
        }

        Amount current_amount = 0;
        const auto balance = accounts_.find_balance(
            identity->account_id,
            objective.asset_id);
        if (balance.has_value()) {
            if (balance->available
                > std::numeric_limits<Amount>::max()
                      - balance->reserved) {
                throw std::overflow_error(
                    "Objective holding amount overflow");
            }
            current_amount = balance->available + balance->reserved;
        }

        return ObjectiveProgress{
            agent_id,
            objective.asset_id,
            current_amount,
            objective.target_amount,
            current_amount >= objective.target_amount};
    }
}  // namespace exchange
