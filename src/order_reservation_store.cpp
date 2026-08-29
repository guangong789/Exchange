#include "exchange/order_reservation_store.hpp"

#include <stdexcept>

namespace exchange {
    bool OrderReservationStore::create(
        OrderId order_id,
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        if (order_id == 0) {
            throw std::invalid_argument("order ID must be non-zero");
        }
        if (account_id == 0) {
            throw std::invalid_argument("account ID must be non-zero");
        }
        if (asset_id == 0) {
            throw std::invalid_argument("asset ID must be non-zero");
        }
        if (amount <= 0) {
            throw std::invalid_argument("reservation amount must be positive");
        }

        return reservations_
            .try_emplace(
                order_id,
                OrderReservation{account_id, asset_id, amount, amount})
            .second;
    }

    std::optional<OrderReservation> OrderReservationStore::find(
        OrderId order_id) const {
        if (order_id == 0) {
            throw std::invalid_argument("order ID must be non-zero");
        }

        const auto reservation = reservations_.find(order_id);
        if (reservation == reservations_.end()) {
            return std::nullopt;
        }

        return reservation->second;
    }

    void OrderReservationStore::consume(OrderId order_id, Amount amount) {
        if (order_id == 0) {
            throw std::invalid_argument("order ID must be non-zero");
        }
        if (amount <= 0) {
            throw std::invalid_argument("consumption amount must be positive");
        }

        const auto reservation = reservations_.find(order_id);
        if (reservation == reservations_.end()) {
            throw std::out_of_range("order reservation does not exist");
        }
        if (amount > reservation->second.remaining_amount) {
            throw std::logic_error("consumption exceeds remaining reservation");
        }

        reservation->second.remaining_amount -= amount;
    }

    OrderReservation OrderReservationStore::remove(OrderId order_id) {
        if (order_id == 0) {
            throw std::invalid_argument("order ID must be non-zero");
        }

        const auto reservation = reservations_.find(order_id);
        if (reservation == reservations_.end()) {
            throw std::out_of_range("order reservation does not exist");
        }

        const OrderReservation removed = reservation->second;
        reservations_.erase(reservation);
        return removed;
    }
}  // namespace exchange
