#pragma once

#include "exchange/accounting/account.hpp"
#include "exchange/core/types.hpp"
#include "exchange/matching/event.hpp"
#include "exchange/matching/order.hpp"

#include <optional>
#include <variant>
#include <vector>

namespace exchange {
    struct SubmitTradingRequest {
        Side side{Side::Buy};
        Price price{};
        Quantity quantity{};
    };

    struct CancelTradingRequest {
        OrderId order_id{};
    };

    using TradingRequestPayload = std::variant<
        SubmitTradingRequest,
        CancelTradingRequest>;

    struct TradingRequest {
        RequestId request_id{};
        AccountId account_id{};
        TradingRequestPayload payload;
    };

    enum class TradingResult {
        Accepted,
        Cancelled,
        AccountNotFound,
        InsufficientFunds,
        DuplicateOrder,
        InvalidOrder,
        CounterpartyNotAccountBacked,
        CancelNotFound,
        CancelNotOwner,
        InvalidRequest,
    };

    struct TradingResponse {
        RequestId request_id{};
        TradingResult result{TradingResult::InvalidRequest};
        std::vector<Event> events;
        std::optional<OrderId> assigned_order_id;
    };
}  // namespace exchange
