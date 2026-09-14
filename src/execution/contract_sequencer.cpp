#include "execution/contract_sequencer.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    ContractId ContractSequencer::allocate() {
        if (next_contract_id_ == std::numeric_limits<ContractId>::max()) {
            throw std::overflow_error("contract sequence is exhausted");
        }
        return next_contract_id_++;
    }

    ContractId ContractSequencer::next_id() const noexcept {
        return next_contract_id_;
    }

    bool ContractSequencer::advance_recovered_id(
        ContractId contract_id) noexcept {
        if (next_contract_id_ == std::numeric_limits<ContractId>::max()
            || contract_id != next_contract_id_) {
            return false;
        }
        ++next_contract_id_;
        return true;
    }
}  // namespace exchange
