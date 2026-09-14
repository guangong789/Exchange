#pragma once

#include "agent/domain/contract.hpp"

namespace exchange {
    class ContractSequencer {
    public:
        ContractSequencer() = default;
        ContractSequencer(const ContractSequencer&) = delete;
        ContractSequencer& operator=(const ContractSequencer&) = delete;
        ContractSequencer(ContractSequencer&&) = delete;
        ContractSequencer& operator=(ContractSequencer&&) = delete;

        [[nodiscard]] ContractId allocate();
        [[nodiscard]] ContractId next_id() const noexcept;
        [[nodiscard]] bool advance_recovered_id(ContractId contract_id)
            noexcept;

    private:
        ContractId next_contract_id_{1};
    };
}  // namespace exchange
