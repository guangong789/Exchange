#include "exchange/matching_engine.hpp"

#include <cstddef>
#include <utility>

namespace exchange {
    MatchingEngine::MatchingEngine(EventCollector& event_collector) noexcept
        : event_collector_(event_collector) {}

    std::vector<Trade> MatchingEngine::add_order(Order order) {
        const Order accepted_order = order;
        auto result = order_book_.add_order_with_execution_result(order);

        event_collector_.publish(Event{EventPayload{OrderAccepted{accepted_order}}});

        Quantity taker_remaining = accepted_order.quantity;
        for (std::size_t trade_index = 0;
             trade_index < result.trades.size();
             ++trade_index) {
            const Trade& trade = result.trades[trade_index];
            event_collector_.publish(Event{EventPayload{TradeCreated{trade}}});

            const bool taker_is_buy = accepted_order.side == Side::Buy;
            const OrderId maker_id = taker_is_buy ? trade.sell_order_id : trade.buy_order_id;
            const Side maker_side = taker_is_buy ? Side::Sell : Side::Buy;
            const bool is_last_trade = trade_index + 1 == result.trades.size();
            const Quantity maker_remaining =
                is_last_trade
                    ? result.last_trade_maker_remaining_quantity
                    : 0;

            publish_fill_state(maker_id, maker_side, trade.quantity, maker_remaining);

            taker_remaining -= trade.quantity;
            publish_fill_state(accepted_order.id, accepted_order.side, trade.quantity, taker_remaining);
        }

        return std::move(result.trades);
    }

    bool MatchingEngine::cancel_order(OrderId order_id) {
        const auto order = order_book_.find_order(order_id);
        const bool cancelled = order_book_.cancel_order(order_id);

        if (cancelled && order.has_value()) {
            event_collector_.publish(Event{EventPayload{OrderCancelled{*order}}});
        }
        return cancelled;
    }

    const OrderBook& MatchingEngine::order_book() const noexcept {
        return order_book_;
    }

    void MatchingEngine::publish_fill_state(OrderId order_id,
                                            Side side,
                                            Quantity filled_quantity,
                                            Quantity remaining_quantity) {
        if (remaining_quantity == 0) {
            event_collector_.publish(Event{EventPayload{OrderFilled{
                order_id, side, filled_quantity}}});
            return;
        }

        event_collector_.publish(Event{EventPayload{OrderPartiallyFilled{
                order_id, side, filled_quantity, remaining_quantity}}});
    }
}  // namespace exchange
