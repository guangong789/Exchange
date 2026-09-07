#pragma once

#include "exchange/core/types.hpp"

namespace exchange {
    struct AssignedOrderIdentity {
        OrderId order_id{};
        Timestamp timestamp{};

        bool operator==(const AssignedOrderIdentity&) const = default;
    };

    class ExecutionSequencer {
    public:
        ExecutionSequencer() = default;
        ExecutionSequencer(const ExecutionSequencer&) = delete;
        ExecutionSequencer& operator=(const ExecutionSequencer&) = delete;
        ExecutionSequencer(ExecutionSequencer&&) = delete;
        ExecutionSequencer& operator=(ExecutionSequencer&&) = delete;

        [[nodiscard]] AssignedOrderIdentity allocate();
        [[nodiscard]] AssignedOrderIdentity next_identity() const noexcept;
        [[nodiscard]] bool advance_recovered_identity(
            AssignedOrderIdentity identity) noexcept;

    private:
        OrderId next_order_id_{1};
        Timestamp next_timestamp_{1};
    };
}  // namespace exchange
