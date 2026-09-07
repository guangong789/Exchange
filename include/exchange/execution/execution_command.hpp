#pragma once

#include "exchange/accounting/account.hpp"
#include "exchange/core/types.hpp"
#include "exchange/matching/order.hpp"

#include <variant>

namespace exchange {
    struct SubmitExecutionCommand {
        RequestId request_id{};
        AccountId account_id{};
        Order order;
    };

    struct CancelExecutionCommand {
        RequestId request_id{};
        AccountId account_id{};
        OrderId order_id{};
    };

    using ExecutionCommand = std::variant<
        SubmitExecutionCommand,
        CancelExecutionCommand>;
}  // namespace exchange
