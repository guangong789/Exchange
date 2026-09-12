#include "execution/execution_sequencer.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    AssignedOrderIdentity ExecutionSequencer::allocate() {
        if (next_order_id_ == std::numeric_limits<OrderId>::max()
            || next_timestamp_ == std::numeric_limits<Timestamp>::max()) {
            throw std::overflow_error("execution sequence is exhausted");
        }

        const AssignedOrderIdentity identity{
            next_order_id_,
            next_timestamp_};
        ++next_order_id_;
        ++next_timestamp_;
        return identity;
    }

    AssignedOrderIdentity ExecutionSequencer::next_identity() const noexcept {
        return AssignedOrderIdentity{next_order_id_, next_timestamp_};
    }

    bool ExecutionSequencer::advance_recovered_identity(
        AssignedOrderIdentity identity) noexcept {
        if (next_order_id_ == std::numeric_limits<OrderId>::max()
            || next_timestamp_ == std::numeric_limits<Timestamp>::max()
            || identity.order_id != next_order_id_
            || identity.timestamp != next_timestamp_) {
            return false;
        }
        ++next_order_id_;
        ++next_timestamp_;
        return true;
    }
}  // namespace exchange
