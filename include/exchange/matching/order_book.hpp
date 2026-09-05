#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "exchange/matching/order.hpp"
#include "exchange/matching/trade.hpp"

namespace exchange {
    struct MatchPreview {
        OrderId maker_order_id{};
        Price execution_price{};
        Quantity execution_quantity{};

        bool operator==(const MatchPreview&) const = default;
    };

    struct AddOrderExecutionResult {
        std::vector<Trade> trades;
        Quantity last_trade_maker_remaining_quantity{};
    };

    class OrderBook {
    public:
        OrderBook() = default;
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) = delete;
        OrderBook& operator=(OrderBook&&) = delete;

        void reserve_order_capacity(std::size_t capacity);

        [[nodiscard]] std::vector<Trade> add_order(Order order);
        [[nodiscard]] AddOrderExecutionResult add_order_with_execution_result(
            Order order);
        [[nodiscard]] std::vector<MatchPreview> preview_matches(
            const Order& incoming) const;

        [[nodiscard]] bool cancel_order(OrderId order_id);
        [[nodiscard]] std::optional<Order> cancel_order_with_result(
            OrderId order_id);

        [[nodiscard]] std::optional<Price> best_bid() const noexcept;
        [[nodiscard]] std::optional<Price> best_ask() const noexcept;
        [[nodiscard]] std::size_t order_count() const noexcept;
        [[nodiscard]] std::optional<Order> find_order(OrderId order_id) const;

    private:
        using OrderQueue = std::list<Order>;
        using BidBook = std::map<Price, OrderQueue, std::greater<Price>>;
        using AskBook = std::map<Price, OrderQueue, std::less<Price>>;

        struct OrderLocation {
            Side side;
            Price price;
            OrderQueue::iterator iterator;
        };

        void validate_order(const Order& order) const;
        void rest_order(Order order);
        [[nodiscard]] bool cancel_order_impl(
            OrderId order_id,
            std::optional<Order>* cancelled_order);
        [[nodiscard]] AddOrderExecutionResult execute_order(Order order);
        void match_buy(Order& incoming, AddOrderExecutionResult& result);
        void match_sell(Order& incoming, AddOrderExecutionResult& result);

        BidBook bids_;
        AskBook asks_;
        std::unordered_map<OrderId, OrderLocation> order_index_;
    };
}  // namespace exchange
