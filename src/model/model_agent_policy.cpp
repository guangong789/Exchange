#include "exchange/model/model_agent_policy.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

#include "exchange/model/model_decision.hpp"

namespace exchange {
    namespace {
        void append_balance(
            std::ostringstream& output,
            const char* name,
            const std::optional<Balance>& balance) {
            output << name << ": ";
            if (!balance.has_value()) {
                output << "null\n";
                return;
            }
            output << "available=" << balance->available
                   << ", reserved=" << balance->reserved << '\n';
        }

        void append_price(
            std::ostringstream& output,
            const char* name,
            const std::optional<Price>& price) {
            output << name << ": ";
            if (price.has_value()) {
                output << *price;
            } else {
                output << "null";
            }
            output << '\n';
        }

        AgentAction to_agent_action(const ModelDecision& decision) {
            return std::visit(
                [](const auto& payload) -> AgentAction {
                    using Decision = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<
                                      Decision,
                                      ModelSubmitOrderDecision>) {
                        return SubmitOrderAction{
                            payload.side,
                            payload.price,
                            payload.quantity};
                    } else if constexpr (std::is_same_v<
                                             Decision,
                                             ModelCancelOrderDecision>) {
                        return CancelOrderAction{payload.order_id};
                    } else {
                        return HoldAction{};
                    }
                },
                decision);
        }
    }  // namespace

    ModelRequest build_model_request(
        const AgentObservation& observation) {
        ModelRequest request;
        request.system_prompt =
            "Choose exactly one action for your own exchange Agent. "
            "You cannot mutate balances directly. Allowed actions are "
            "submit_order, cancel_order, and hold. Orders are limit orders. "
            "BUY spends quote asset; SELL spends base asset. Available and "
            "reserved balances are distinct, while objective holding counts "
            "both. The deterministic exchange may reject invalid actions. "
            "Return exactly one JSON object, with no Markdown or prose. "
            "Allowed schemas: "
            "{\"action\":\"submit_order\",\"side\":\"buy|sell\","
            "\"price\":positive_integer,\"quantity\":positive_integer}, "
            "{\"action\":\"cancel_order\",\"order_id\":positive_integer}, "
            "or {\"action\":\"hold\"}. Do not include other fields.";

        std::ostringstream user;
        user << "Agent ID: " << observation.agent_id << '\n'
             << "Account ID: " << observation.account_id << '\n';
        append_balance(user, "Base balance", observation.base_balance);
        append_balance(user, "Quote balance", observation.quote_balance);
        append_price(user, "Best bid", observation.best_bid);
        append_price(user, "Best ask", observation.best_ask);
        user << "External market: ";
        if (!observation.external_market.has_value()) {
            user << "null\n";
        } else {
            const ExternalMarketSnapshot& external =
                *observation.external_market;
            user << "symbol=" << external.symbol
                 << ", best bid=" << external.best_bid
                 << ", best ask=" << external.best_ask << '\n';
        }
        user << "Objective: ";
        if (!observation.objective.has_value()) {
            user << "null\n";
        } else {
            const ObjectiveProgress& objective = *observation.objective;
            user << "target asset=" << objective.asset_id
                 << ", current holding=" << objective.current_amount
                 << ", target holding=" << objective.target_amount
                 << ", achieved="
                 << (objective.achieved ? "true" : "false") << '\n';
        }
        user << "Choose one action now.";
        request.user_prompt = user.str();
        return request;
    }

    ModelAgentPolicy::ModelAgentPolicy(ModelAdapter& adapter) noexcept
        : adapter_(adapter) {}

    AgentAction ModelAgentPolicy::decide(
        const AgentObservation& observation) const {
        return decide(observation, {});
    }

    AgentAction ModelAgentPolicy::decide(
        const AgentObservation& observation,
        std::string_view additional_context) const {
        ModelRequest request = build_model_request(observation);
        if (!additional_context.empty()) {
            request.user_prompt.append("\nScenario context:\n");
            request.user_prompt.append(additional_context);
        }
        const ModelResponse response = adapter_.invoke(request);
        return to_agent_action(parse_model_decision(response.content));
    }
}  // namespace exchange
