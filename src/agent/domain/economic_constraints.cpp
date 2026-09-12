#include "agent/domain/economic_constraints.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    namespace {
        Amount current_base_position(
            const AgentObservation& observation) {
            if (!observation.base_balance.has_value()) {
                return 0;
            }
            const Balance& balance = *observation.base_balance;
            if (balance.available < 0 || balance.reserved < 0) {
                throw std::logic_error(
                    "Agent observation has a negative base balance");
            }
            if (balance.available
                > std::numeric_limits<Amount>::max()
                      - balance.reserved) {
                throw std::overflow_error(
                    "Agent base position overflow");
            }
            return balance.available + balance.reserved;
        }

        bool adding_exceeds_limit(
            Amount current,
            Amount addition,
            Amount limit) {
            if (current < 0 || addition < 0 || limit < 0) {
                throw std::logic_error(
                    "Agent position inputs must be non-negative");
            }
            return current > limit || addition > limit - current;
        }
    }  // namespace

    void validate_agent_economic_profile(
        const AgentEconomicProfile& profile) {
        if (profile.max_order_quantity <= 0) {
            throw std::invalid_argument(
                "Agent maximum order quantity must be positive");
        }
        if (profile.max_order_notional <= 0) {
            throw std::invalid_argument(
                "Agent maximum order notional must be positive");
        }
        if (profile.max_base_position < 0) {
            throw std::invalid_argument(
                "Agent maximum base position must be non-negative");
        }
        if (profile.max_buy_price.has_value()
            && *profile.max_buy_price <= 0) {
            throw std::invalid_argument(
                "Agent maximum buy price must be positive");
        }
        if (profile.min_sell_price.has_value()
            && *profile.min_sell_price <= 0) {
            throw std::invalid_argument(
                "Agent minimum sell price must be positive");
        }
    }

    AgentEconomicConstraintResult evaluate_agent_economic_constraints(
        const AgentAction& action,
        const AgentObservation& observation,
        const AgentEconomicProfile& profile,
        const InstrumentContext& instrument) {
        validate_agent_economic_profile(profile);
        validate_instrument_context(instrument);

        return std::visit(
            [&](const auto& payload) {
                using Action = std::decay_t<decltype(payload)>;
                if constexpr (!std::is_same_v<Action, SubmitOrderAction>) {
                    return AgentEconomicConstraintResult::Allowed;
                } else {
                    if (payload.quantity > profile.max_order_quantity) {
                        return AgentEconomicConstraintResult::
                            OrderQuantityExceeded;
                    }

                    const TradeFinancialAmounts amounts =
                        calculate_trade_amounts(
                            instrument,
                            payload.price,
                            payload.quantity);
                    if (amounts.quote_amount
                        > profile.max_order_notional) {
                        return AgentEconomicConstraintResult::
                            OrderNotionalExceeded;
                    }

                    if (payload.side == Side::Buy
                        && profile.max_buy_price.has_value()
                        && payload.price > *profile.max_buy_price) {
                        return AgentEconomicConstraintResult::
                            BuyPriceExceeded;
                    }
                    if (payload.side == Side::Sell
                        && profile.min_sell_price.has_value()
                        && payload.price < *profile.min_sell_price) {
                        return AgentEconomicConstraintResult::
                            SellPriceBelowMinimum;
                    }

                    if (payload.side != Side::Buy) {
                        return AgentEconomicConstraintResult::Allowed;
                    }

                    Amount potential_position =
                        current_base_position(observation);
                    for (const ObservedOrder& order :
                         observation.active_orders) {
                        if (order.side == Side::Sell) {
                            continue;
                        }
                        if (order.side != Side::Buy) {
                            throw std::logic_error(
                                "Agent observation has an invalid order side");
                        }
                        const Amount open_buy_base =
                            calculate_trade_amounts(
                                instrument,
                                order.price,
                                order.remaining_quantity)
                                .base_amount;
                        if (adding_exceeds_limit(
                                potential_position,
                                open_buy_base,
                                profile.max_base_position)) {
                            return AgentEconomicConstraintResult::
                                BasePositionExceeded;
                        }
                        potential_position += open_buy_base;
                    }
                    if (adding_exceeds_limit(
                            potential_position,
                            amounts.base_amount,
                            profile.max_base_position)) {
                        return AgentEconomicConstraintResult::
                            BasePositionExceeded;
                    }
                    return AgentEconomicConstraintResult::Allowed;
                }
            },
            action);
    }
}  // namespace exchange
