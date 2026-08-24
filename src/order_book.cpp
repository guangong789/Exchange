#include "exchange/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace exchange {
    std::vector<Trade> OrderBook::add_order(Order order) {
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

        std::vector<Trade> trades;  // 交易数据
        if (order.side == Side::Buy) {
            match_buy(order, trades);
        } else {
            match_sell(order, trades);
        }

        if (order.quantity > 0) {
            rest_order(order);
        }
        return trades;
    }

    void OrderBook::match_buy(Order& incoming, std::vector<Trade>& trades) {
        while (incoming.quantity > 0 && !asks_.empty() && asks_.begin()->first <= incoming.price) {
            auto level = asks_.begin();
            auto& queue = level->second;

            while (incoming.quantity > 0 && !queue.empty()) {
                Order& resting = queue.front();
                const Quantity executed = std::min(incoming.quantity, resting.quantity);
                trades.push_back(Trade{incoming.id, resting.id, resting.price, executed, incoming.timestamp});
                incoming.quantity -= executed;
                resting.quantity -= executed;

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

    void OrderBook::match_sell(Order& incoming, std::vector<Trade>& trades) {
        while (incoming.quantity > 0 && !bids_.empty() && bids_.begin()->first >= incoming.price) {
            auto level = bids_.begin();
            auto& queue = level->second;

            while (incoming.quantity > 0 && !queue.empty()) {
                Order& resting = queue.front();
                const Quantity executed = std::min(incoming.quantity, resting.quantity);
                trades.push_back(Trade{resting.id, incoming.id, resting.price, executed, incoming.timestamp});
                incoming.quantity -= executed;
                resting.quantity -= executed;

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
        const auto location = order_index_.find(order_id);
        if (location == order_index_.end()) {
            return false;
        }

        const Side side = location->second.side;
        const Price price = location->second.price;
        const auto order = location->second.iterator;

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