#include "agent/domain/utility.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace exchange {
    namespace {
        Amount owned_amount(
            const std::optional<Balance>& balance,
            std::string_view name) {
            if (!balance.has_value()) {
                return 0;
            }
            if (balance->available < 0 || balance->reserved < 0) {
                throw std::logic_error(
                    "Agent utility has a negative " + std::string{name}
                    + " balance");
            }
            if (balance->available
                > std::numeric_limits<Amount>::max()
                      - balance->reserved) {
                throw std::overflow_error(
                    "Agent utility balance overflow");
            }
            return balance->available + balance->reserved;
        }

        UtilityValue multiply_non_negative(
            Amount left,
            Amount right,
            const char* message) {
            if (left < 0 || right < 0) {
                throw std::logic_error(
                    "Agent utility inputs must be non-negative");
            }
            if (right != 0
                && left > std::numeric_limits<UtilityValue>::max() / right) {
                throw std::overflow_error(message);
            }
            return left * right;
        }

        UtilityValue add_non_negative(
            UtilityValue left,
            UtilityValue right) {
            if (left < 0 || right < 0) {
                throw std::logic_error(
                    "Agent utility components must be non-negative");
            }
            if (left > std::numeric_limits<UtilityValue>::max() - right) {
                throw std::overflow_error("Agent utility total overflow");
            }
            return left + right;
        }
    }  // namespace

    void validate_agent_preference_profile(
        const AgentPreferenceProfile& profile) {
        if (profile.target_base_inventory < 0
            || profile.base_unit_value < 0
            || profile.inventory_deviation_penalty_per_unit < 0) {
            throw std::invalid_argument(
                "Agent preference values must be non-negative");
        }
    }

    AgentUtilityBreakdown evaluate_agent_utility(
        const AgentObservation& observation,
        const AgentPreferenceProfile& profile) {
        validate_agent_preference_profile(profile);
        const Amount base_inventory =
            owned_amount(observation.base_balance, "base");
        const Amount quote_inventory =
            owned_amount(observation.quote_balance, "quote");
        const Amount deviation = base_inventory
                >= profile.target_base_inventory
            ? base_inventory - profile.target_base_inventory
            : profile.target_base_inventory - base_inventory;
        const UtilityValue base_value = multiply_non_negative(
            base_inventory,
            profile.base_unit_value,
            "Agent base valuation overflow");
        const UtilityValue penalty = multiply_non_negative(
            deviation,
            profile.inventory_deviation_penalty_per_unit,
            "Agent inventory penalty overflow");
        const UtilityValue gross = add_non_negative(
            quote_inventory,
            base_value);
        return AgentUtilityBreakdown{
            quote_inventory,
            base_value,
            penalty,
            gross - penalty};
    }

    UtilityValue calculate_agent_utility_delta(
        UtilityValue before,
        UtilityValue after) {
        if ((before > 0
             && after < std::numeric_limits<UtilityValue>::min() + before)
            || (before < 0
                && after
                    > std::numeric_limits<UtilityValue>::max() + before)) {
            throw std::overflow_error("Agent utility delta overflow");
        }
        return after - before;
    }
}  // namespace exchange
