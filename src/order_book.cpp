#include "exchange/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace exchange {
    void OrderBook::reserve_order_capacity(std::size_t capacity) {
        order_index_.reserve(capacity);
    }

    std::vector<Trade> OrderBook::add_order(Order order) {
        auto result = execute_order(order);
        return std::move(result.trades);
    }

    AddOrderExecutionResult OrderBook::add_order_with_execution_result(
        Order order) {
        return execute_order(order);
    }

    AddOrderExecutionResult OrderBook::execute_order(Order order) {
        if (order.id == 0) {
            throw std::invalid_argument("order id must be non-zero");
        }
        if (order.price <= 0) {
            throw std::invalid_argument("price must be positive");
        }
        if (order.quantity <= 0) {
            throw std::invalid_argument("quantity must be positive");
        }
        if (order.type != OrderType::Limit) {
            throw std::invalid_argument("unsupported order type");
        }
        if (order_index_.contains(order.id)) {
            throw std::invalid_argument("duplicate order id");
        }

        AddOrderExecutionResult result;
        if (order.side == Side::Buy) {
            match_buy(order, result);
        } else {
            match_sell(order, result);
        }

        if (order.quantity > 0) {
            rest_order(order);
        }
        return result;
    }

    void OrderBook::match_buy(Order& incoming,
                              AddOrderExecutionResult& result) {
        while (incoming.quantity > 0 && !asks_.empty() && asks_.begin()->first <= incoming.price) {
            auto level = asks_.begin();
            auto& queue = level->second;

            while (incoming.quantity > 0 && !queue.empty()) {
                Order& resting = queue.front();
                const Quantity executed = std::min(incoming.quantity, resting.quantity);
                result.trades.push_back(Trade{incoming.id, resting.id, resting.price, executed, incoming.timestamp});
                incoming.quantity -= executed;
                resting.quantity -= executed;
                result.last_trade_maker_remaining_quantity = resting.quantity;

                if (resting.quantity == 0) {
                    order_index_.erase(resting.id);
                    queue.pop_front();
                }
            }

            if (queue.empty()) {
                asks_.erase(level);
            }
        }
    }

    void OrderBook::match_sell(Order& incoming,
                               AddOrderExecutionResult& result) {
        while (incoming.quantity > 0 && !bids_.empty() && bids_.begin()->first >= incoming.price) {
            auto level = bids_.begin();
            auto& queue = level->second;

            while (incoming.quantity > 0 && !queue.empty()) {
                Order& resting = queue.front();
                const Quantity executed = std::min(incoming.quantity, resting.quantity);
                result.trades.push_back(Trade{resting.id, incoming.id, resting.price, executed, incoming.timestamp});
                incoming.quantity -= executed;
                resting.quantity -= executed;
                result.last_trade_maker_remaining_quantity = resting.quantity;

                if (resting.quantity == 0) {
                    order_index_.erase(resting.id);
                    queue.pop_front();
                }
            }

            if (queue.empty()) {
                bids_.erase(level);
            }
        }
    }

    void OrderBook::rest_order(Order order) {
        if (order.side == Side::Buy) {
            auto& queue = bids_[order.price];
            queue.push_back(order);
            order_index_.emplace(order.id, OrderLocation{order.side, order.price, std::prev(queue.end())});
        } else {
            auto& queue = asks_[order.price];
            queue.push_back(order);
            order_index_.emplace(order.id, OrderLocation{order.side, order.price, std::prev(queue.end())});
        }
    }

    bool OrderBook::cancel_order(OrderId order_id) {
        return cancel_order_impl(order_id, nullptr);
    }

    std::optional<Order> OrderBook::cancel_order_with_result(
        OrderId order_id) {
        std::optional<Order> cancelled_order;
        if (!cancel_order_impl(order_id, &cancelled_order)) {
            return std::nullopt;
        }
        return cancelled_order;
    }

    bool OrderBook::cancel_order_impl(
        OrderId order_id,
        std::optional<Order>* cancelled_order) {
        const auto location = order_index_.find(order_id);
        if (location == order_index_.end()) {
            return false;
        }

        const Side side = location->second.side;
        const Price price = location->second.price;
        const auto order = location->second.iterator;
        if (cancelled_order != nullptr) {
            cancelled_order->emplace(*order);
        }

        if (side == Side::Buy) {
            auto level = bids_.find(price);
            level->second.erase(order);
            if (level->second.empty()) {
                bids_.erase(level);
            }
        } else {
            auto level = asks_.find(price);
            level->second.erase(order);
            if (level->second.empty()) {
                asks_.erase(level);
            }
        }
        order_index_.erase(location);
        return true;
    }

    std::optional<Price> OrderBook::best_bid() const noexcept {
        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    std::optional<Price> OrderBook::best_ask() const noexcept {
        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    std::size_t OrderBook::order_count() const noexcept {
        return order_index_.size();
    }

    std::optional<Order> OrderBook::find_order(OrderId order_id) const {
        const auto location = order_index_.find(order_id);
        if (location == order_index_.end()) {
            return std::nullopt;
        }
        return *location->second.iterator;
    }
}  // namespace exchange
