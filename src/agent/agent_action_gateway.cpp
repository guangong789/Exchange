#include "exchange/agent/agent_action_gateway.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    AgentActionGateway::AgentActionGateway(
        const AgentRegistry& registry,
        ExecutionCoordinator& execution_coordinator) noexcept
        : registry_(registry),
          execution_coordinator_(execution_coordinator) {}

    AgentActionResult AgentActionGateway::execute(
        AgentId agent_id,
        const AgentAction& action) {
        const auto identity = registry_.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("Agent does not exist");
        }

        return std::visit(
            [this, account_id = identity->account_id](
                const auto& payload) -> AgentActionResult {
                using Action = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
                    if (next_order_id_
                            == std::numeric_limits<OrderId>::max()
                        || next_timestamp_
                               == std::numeric_limits<Timestamp>::max()) {
                        throw std::overflow_error(
                            "Agent action sequence is exhausted");
                    }

                    const OrderId order_id = next_order_id_++;
                    const Timestamp timestamp = next_timestamp_++;
                    const SubmitResult result =
                        execution_coordinator_.submit_order(
                            OrderAdmissionRequest{
                                account_id,
                                Order{
                                    order_id,
                                    payload.side,
                                    OrderType::Limit,
                                    payload.price,
                                    payload.quantity,
                                    timestamp}});
                    return SubmitActionResult{
                        order_id,
                        timestamp,
                        result};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         CancelOrderAction>) {
                    return CancelActionResult{
                        execution_coordinator_.cancel_order(
                            account_id,
                            payload.order_id)};
                } else {
                    return HoldActionResult{};
                }
            },
            action);
    }
}  // namespace exchange
