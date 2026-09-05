#pragma once

#include "exchange/core/types.hpp"
#include "exchange/matching/order.hpp"
#include "exchange/matching/trade.hpp"

#include <cstddef>
#include <vector>

#include "exchange/matching/event_collector.hpp"
#include "exchange/matching/order_book.hpp"

namespace exchange {
    class MatchingEngine {
    public:
        explicit MatchingEngine(EventCollector& event_collector) noexcept;

        MatchingEngine(const MatchingEngine&) = delete;
        MatchingEngine& operator=(const MatchingEngine&) = delete;
        MatchingEngine(MatchingEngine&&) = delete;
        MatchingEngine& operator=(MatchingEngine&&) = delete;

        void reserve_order_capacity(std::size_t capacity);

        [[nodiscard]] std::vector<Trade> add_order(Order order);
        [[nodiscard]] bool cancel_order(OrderId order_id);

        [[nodiscard]] const OrderBook& order_book() const noexcept;

    private:
        void publish_fill_state(OrderId order_id,
                                Side side,
                                Quantity filled_quantity,
                                Quantity remaining_quantity);

        EventCollector& event_collector_;
        OrderBook order_book_;
    };
}  // namespace exchange
