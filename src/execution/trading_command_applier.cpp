#include "execution/trading_command_applier.hpp"

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace exchange {
    namespace {
        TradingResult map_submit_result(SubmitResult result) {
            switch (result) {
                case SubmitResult::Accepted:
                    return TradingResult::Accepted;
                case SubmitResult::AccountNotFound:
                    return TradingResult::AccountNotFound;
                case SubmitResult::InsufficientFunds:
                    return TradingResult::InsufficientFunds;
                case SubmitResult::DuplicateOrder:
                    return TradingResult::DuplicateOrder;
                case SubmitResult::InvalidOrder:
                    return TradingResult::InvalidOrder;
                case SubmitResult::CounterpartyNotAccountBacked:
                    return TradingResult::CounterpartyNotAccountBacked;
            }
            throw std::logic_error("unknown submit result");
        }

        TradingResult map_cancel_result(CancelResult result) {
            switch (result) {
                case CancelResult::Cancelled:
                    return TradingResult::Cancelled;
                case CancelResult::AccountNotFound:
                    return TradingResult::AccountNotFound;
                case CancelResult::NotFound:
                    return TradingResult::CancelNotFound;
                case CancelResult::NotOwner:
                    return TradingResult::CancelNotOwner;
            }
            throw std::logic_error("unknown cancel result");
        }
    }  // namespace

    TradingCommandApplier::TradingCommandApplier(
        ExecutionCoordinator& execution_coordinator,
        EventCollector& events) noexcept
        : execution_coordinator_(execution_coordinator),
          events_(events) {}

    TradingResponse TradingCommandApplier::apply(
        const ExecutionCommand& command) {
        events_.clear();
        return std::visit(
            [this](const auto& payload) -> TradingResponse {
                using Command = std::decay_t<decltype(payload)>;

                if constexpr (std::is_same_v<
                                  Command,
                                  SubmitExecutionCommand>) {
                    const TradingResult result = map_submit_result(
                        execution_coordinator_.submit_order(
                            OrderAdmissionRequest{
                                payload.account_id,
                                payload.order}));
                    return TradingResponse{
                        payload.request_id,
                        result,
                        events_.events(),
                        result == TradingResult::Accepted
                            ? std::optional<OrderId>{payload.order.id}
                            : std::nullopt};
                } else if constexpr (std::is_same_v<
                                         Command,
                                         CancelExecutionCommand>) {
                    return TradingResponse{
                        payload.request_id,
                        map_cancel_result(
                            execution_coordinator_.cancel_order(
                                payload.account_id,
                                payload.order_id)),
                        events_.events(),
                        std::nullopt};
                } else {
                    throw std::logic_error(
                        "contract command passed to trading applier");
                }
            },
            command);
    }
}  // namespace exchange
