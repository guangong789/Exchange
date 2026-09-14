#include "agent/domain/action_validation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    AgentActionValidationResult validate_agent_action(
        const AgentAction& action,
        const AgentObservation& observation,
        const InstrumentContext& instrument) {
        return std::visit(
            [&](const auto& payload) {
                using Action = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
                    if (payload.side != Side::Buy
                        && payload.side != Side::Sell) {
                        return AgentActionValidationResult::InvalidSide;
                    }
                    if (payload.price <= 0) {
                        return AgentActionValidationResult::InvalidPrice;
                    }
                    if (payload.quantity <= 0) {
                        return AgentActionValidationResult::InvalidQuantity;
                    }
                    try {
                        static_cast<void>(calculate_order_reservation(
                            instrument,
                            payload.side,
                            payload.price,
                            payload.quantity));
                    } catch (const std::invalid_argument&) {
                        return AgentActionValidationResult::InvalidFinancialValue;
                    } catch (const std::overflow_error&) {
                        return AgentActionValidationResult::InvalidFinancialValue;
                    }
                    return AgentActionValidationResult::Valid;
                } else if constexpr (std::is_same_v<
                                         Action,
                                         CancelOrderAction>) {
                    if (payload.order_id == 0) {
                        return AgentActionValidationResult::InvalidOrderId;
                    }
                    const bool active = std::ranges::any_of(
                        observation.active_orders,
                        [&](const ObservedOrder& order) {
                            return order.order_id == payload.order_id;
                        });
                    return active
                        ? AgentActionValidationResult::Valid
                        : AgentActionValidationResult::CancelTargetNotActive;
                } else if constexpr (std::is_same_v<
                                         Action,
                                         ProposeContractAction>) {
                    if (payload.counterparty == 0
                        || payload.counterparty == observation.agent_id) {
                        return AgentActionValidationResult::
                            InvalidContractCounterparty;
                    }
                    const ContractTerms& terms = payload.terms;
                    const bool valid_parties =
                        (terms.payer == observation.agent_id
                         && terms.payee == payload.counterparty)
                        || (terms.payer == payload.counterparty
                            && terms.payee == observation.agent_id);
                    if (observation.agent_id == 0 || !valid_parties) {
                        return AgentActionValidationResult::
                            InvalidContractParties;
                    }
                    if (terms.quote_payment_amount <= 0) {
                        return AgentActionValidationResult::
                            InvalidContractPayment;
                    }
                    if (terms.resource != ResourceKind::ComputeCredit) {
                        return AgentActionValidationResult::
                            InvalidContractResource;
                    }
                    if (terms.resource_quantity <= 0) {
                        return AgentActionValidationResult::
                            InvalidContractQuantity;
                    }
                    return AgentActionValidationResult::Valid;
                } else if constexpr (std::is_same_v<
                                         Action,
                                         AcceptContractAction>
                                     || std::is_same_v<
                                         Action,
                                         RejectContractAction>
                                     || std::is_same_v<
                                         Action,
                                         FulfillResourceObligationAction>
                                     || std::is_same_v<
                                         Action,
                                         SettlePaymentObligationAction>) {
                    return payload.contract_id == 0
                            || payload.contract_id
                                == std::numeric_limits<ContractId>::max()
                        ? AgentActionValidationResult::InvalidContractId
                        : AgentActionValidationResult::Valid;
                } else {
                    return AgentActionValidationResult::Valid;
                }
            },
            action);
    }
}  // namespace exchange
