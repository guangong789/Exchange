#pragma once

#include <stdexcept>
#include <string_view>
#include <variant>

#include "exchange/core/types.hpp"

namespace exchange {
    struct ModelSubmitOrderDecision {
        Side side{Side::Buy};
        Price price{};
        Quantity quantity{};

        bool operator==(const ModelSubmitOrderDecision&) const = default;
    };

    struct ModelCancelOrderDecision {
        OrderId order_id{};

        bool operator==(const ModelCancelOrderDecision&) const = default;
    };

    struct ModelHoldDecision {
        bool operator==(const ModelHoldDecision&) const = default;
    };

    using ModelDecision = std::variant<
        ModelSubmitOrderDecision,
        ModelCancelOrderDecision,
        ModelHoldDecision>;

    class ModelDecisionParseError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    [[nodiscard]] ModelDecision parse_model_decision(
        std::string_view content);
}  // namespace exchange
