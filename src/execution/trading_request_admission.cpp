#include "execution/trading_request_admission.hpp"

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    namespace {
        bool has_valid_submit_values(
            const SubmitTradingRequest& request,
            const InstrumentContext& instrument) {
            if ((request.side != Side::Buy && request.side != Side::Sell)
                || request.price <= 0 || request.quantity <= 0) {
                return false;
            }

            // Keep invalid static configuration fatal while translating only
            // failures produced by these otherwise valid client values.
            validate_instrument_context(instrument);
            try {
                static_cast<void>(calculate_order_reservation(
                    instrument,
                    request.side,
                    request.price,
                    request.quantity));
            } catch (const std::invalid_argument&) {
                return false;
            } catch (const std::overflow_error&) {
                return false;
            }
            return true;
        }
    }  // namespace

    ExecutionAdmissionResult admit_trading_request(
        const TradingRequest& request,
        const InstrumentContext& instrument,
        ExecutionSequencer& sequencer) {
        if (request.request_id == 0 || request.account_id == 0) {
            return TradingResult::InvalidRequest;
        }

        return std::visit(
            [&request, &instrument, &sequencer](
                const auto& payload) -> ExecutionAdmissionResult {
                using Request = std::decay_t<decltype(payload)>;

                if constexpr (std::is_same_v<Request, SubmitTradingRequest>) {
                    if (!has_valid_submit_values(payload, instrument)) {
                        return TradingResult::InvalidOrder;
                    }

                    const AssignedOrderIdentity identity =
                        sequencer.allocate();
                    return ExecutionCommand{SubmitExecutionCommand{
                        request.request_id,
                        request.account_id,
                        Order{
                            identity.order_id,
                            payload.side,
                            OrderType::Limit,
                            payload.price,
                            payload.quantity,
                            identity.timestamp}}};
                } else {
                    if (payload.order_id == 0) {
                        return TradingResult::InvalidOrder;
                    }
                    return ExecutionCommand{CancelExecutionCommand{
                        request.request_id,
                        request.account_id,
                        payload.order_id}};
                }
            },
            request.payload);
    }
}  // namespace exchange
