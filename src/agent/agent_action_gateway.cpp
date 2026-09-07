#include "exchange/agent/agent_action_gateway.hpp"

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    AgentActionGateway::AgentActionGateway(
        const AgentRegistry& registry,
        ExecutionCoordinator& execution_coordinator,
        ExecutionSequencer& sequencer) noexcept
        : registry_(registry),
          execution_coordinator_(execution_coordinator),
          sequencer_(sequencer) {}

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
                    if ((payload.side != Side::Buy
                            && payload.side != Side::Sell)
                        || payload.price <= 0 || payload.quantity <= 0) {
                        return SubmitActionResult{
                            0,
                            0,
                            SubmitResult::InvalidOrder};
                    }
                    const AssignedOrderIdentity identity =
                        sequencer_.allocate();
                    const SubmitResult result =
                        execution_coordinator_.submit_order(
                            OrderAdmissionRequest{
                                account_id,
                                Order{
                                    identity.order_id,
                                    payload.side,
                                    OrderType::Limit,
                                    payload.price,
                                    payload.quantity,
                                    identity.timestamp}});
                    return SubmitActionResult{
                        identity.order_id,
                        identity.timestamp,
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
