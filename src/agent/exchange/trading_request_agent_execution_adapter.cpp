#include "agent/exchange/trading_request_agent_execution_adapter.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    namespace {
        AgentSubmitStatus map_submit_status(TradingResult result) {
            switch (result) {
                case TradingResult::Accepted:
                    return AgentSubmitStatus::Accepted;
                case TradingResult::AccountNotFound:
                    return AgentSubmitStatus::AccountNotFound;
                case TradingResult::InsufficientFunds:
                    return AgentSubmitStatus::InsufficientFunds;
                case TradingResult::DuplicateOrder:
                    return AgentSubmitStatus::DuplicateOrder;
                case TradingResult::InvalidOrder:
                case TradingResult::InvalidRequest:
                    return AgentSubmitStatus::InvalidOrder;
                case TradingResult::CounterpartyNotAccountBacked:
                    return AgentSubmitStatus::CounterpartyNotAccountBacked;
                case TradingResult::Cancelled:
                case TradingResult::CancelNotFound:
                case TradingResult::CancelNotOwner:
                    break;
            }
            throw std::logic_error("Unexpected submit trading result");
        }

        AgentCancelStatus map_cancel_status(TradingResult result) {
            switch (result) {
                case TradingResult::Cancelled:
                    return AgentCancelStatus::Cancelled;
                case TradingResult::AccountNotFound:
                    return AgentCancelStatus::AccountNotFound;
                case TradingResult::CancelNotFound:
                case TradingResult::InvalidRequest:
                    return AgentCancelStatus::NotFound;
                case TradingResult::CancelNotOwner:
                    return AgentCancelStatus::NotOwner;
                case TradingResult::Accepted:
                case TradingResult::InsufficientFunds:
                case TradingResult::DuplicateOrder:
                case TradingResult::InvalidOrder:
                case TradingResult::CounterpartyNotAccountBacked:
                    break;
            }
            throw std::logic_error("Unexpected cancel trading result");
        }

        Timestamp accepted_timestamp(const TradingResponse& response) {
            for (const Event& event : response.events) {
                if (const auto* accepted =
                        std::get_if<OrderAccepted>(&event.payload)) {
                    return accepted->order.timestamp;
                }
            }
            throw std::logic_error(
                "Accepted Agent submit has no acceptance event");
        }
    }  // namespace

    TradingRequestAgentExecutionAdapter::
        TradingRequestAgentExecutionAdapter(
            const AgentRegistry& registry,
            TradingRequestExecutor& executor,
            ContractRequestExecutor& contract_executor,
            RequestId first_request_id)
        : registry_(registry),
          executor_(executor),
          contract_executor_(contract_executor),
          next_request_id_(first_request_id) {
        if (first_request_id == 0) {
            throw std::invalid_argument(
                "Agent request ID must be non-zero");
        }
    }

    AgentActionResult TradingRequestAgentExecutionAdapter::execute(
        AgentId agent_id,
        const AgentAction& action) {
        const auto identity = registry_.find(agent_id);
        if (!identity.has_value()) {
            throw std::out_of_range("Agent does not exist");
        }

        return std::visit(
            [&](const auto& payload) -> AgentActionResult {
                using Action = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Action, HoldAction>) {
                    return HoldActionResult{};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         SubmitOrderAction>) {
                    const TradingResponse response = executor_.execute(
                        TradingRequest{
                            allocate_request_id(),
                            identity->account_id,
                            SubmitTradingRequest{
                                payload.side,
                                payload.price,
                                payload.quantity}});
                    const AgentSubmitStatus status =
                        map_submit_status(response.result);
                    if (status != AgentSubmitStatus::Accepted) {
                        return SubmitActionResult{0, 0, status};
                    }
                    if (!response.assigned_order_id.has_value()) {
                        throw std::logic_error(
                            "Accepted Agent submit has no order ID");
                    }
                    return SubmitActionResult{
                        *response.assigned_order_id,
                        accepted_timestamp(response),
                        status};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         CancelOrderAction>) {
                    const TradingResponse response = executor_.execute(
                        TradingRequest{
                            allocate_request_id(),
                            identity->account_id,
                            CancelTradingRequest{payload.order_id}});
                    return CancelActionResult{
                        map_cancel_status(response.result)};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         ProposeContractAction>) {
                    const ContractExecutionResponse response =
                        contract_executor_.create_contract(
                            CreateContractRequest{
                                agent_id,
                                payload.counterparty,
                                payload.terms});
                    return ContractActionResult{
                        response.contract_id,
                        response.result};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         AcceptContractAction>) {
                    return ContractActionResult{
                        std::optional<ContractId>{payload.contract_id},
                        contract_executor_.accept_contract(
                            payload.contract_id,
                            agent_id)};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         RejectContractAction>) {
                    return ContractActionResult{
                        std::optional<ContractId>{payload.contract_id},
                        contract_executor_.reject_contract(
                            payload.contract_id,
                            agent_id)};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         FulfillResourceObligationAction>) {
                    return ContractActionResult{
                        std::optional<ContractId>{payload.contract_id},
                        contract_executor_.fulfill_resource(
                            payload.contract_id,
                            agent_id)};
                } else if constexpr (std::is_same_v<
                                         Action,
                                         SettlePaymentObligationAction>) {
                    return ContractActionResult{
                        std::optional<ContractId>{payload.contract_id},
                        contract_executor_.settle_payment(
                            payload.contract_id,
                            agent_id)};
                } else {
                    static_assert(std::is_same_v<Action, HoldAction>);
                    return HoldActionResult{};
                }
            },
            action);
    }

    RequestId TradingRequestAgentExecutionAdapter::allocate_request_id() {
        if (next_request_id_ == 0) {
            throw std::overflow_error("Agent request ID overflow");
        }
        const RequestId allocated = next_request_id_;
        if (next_request_id_ == std::numeric_limits<RequestId>::max()) {
            next_request_id_ = 0;
        } else {
            ++next_request_id_;
        }
        return allocated;
    }
}  // namespace exchange
