#pragma once

#include <map>
#include <optional>

#include "exchange/account.hpp"
#include "exchange/types.hpp"

namespace exchange {
    struct OrderReservation {
        AccountId account_id{};
        AssetId asset_id{};
        Amount original_amount{};
        Amount remaining_amount{};

        bool operator==(const OrderReservation&) const = default;
    };

    class OrderReservationStore {
    public:
        OrderReservationStore() = default;
        OrderReservationStore(const OrderReservationStore&) = delete;
        OrderReservationStore& operator=(const OrderReservationStore&) = delete;
        OrderReservationStore(OrderReservationStore&&) = delete;
        OrderReservationStore& operator=(OrderReservationStore&&) = delete;

        [[nodiscard]] bool create(
            OrderId order_id,
            AccountId account_id,
            AssetId asset_id,
            Amount amount);

        [[nodiscard]] std::optional<OrderReservation> find(
            OrderId order_id) const;

        void consume(OrderId order_id, Amount amount);

        // Removes metadata only. AccountStore balance changes are coordinated
        // by a higher-level component.
        [[nodiscard]] OrderReservation remove(OrderId order_id);

    private:
        std::map<OrderId, OrderReservation> reservations_;
    };
}  // namespace exchange
