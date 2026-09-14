#pragma once

#include "accounting/account_store.hpp"
#include "accounting/ledger.hpp"
#include "agent/domain/contract_store.hpp"
#include "execution/execution_command.hpp"

namespace exchange {
    class ContractCommandApplier {
    public:
        ContractCommandApplier(
            ContractStore& store,
            AccountStore& accounts,
            Ledger& ledger,
            AssetId quote_asset_id);

        ContractCommandApplier(const ContractCommandApplier&) = delete;
        ContractCommandApplier& operator=(const ContractCommandApplier&) = delete;
        ContractCommandApplier(ContractCommandApplier&&) = delete;
        ContractCommandApplier& operator=(ContractCommandApplier&&) = delete;

        [[nodiscard]] ContractResult apply(
            const ExecutionCommand& command);
        [[nodiscard]] ContractResult validate_settlement(
            const SettlePaymentObligationExecutionCommand& command) const;

    private:
        [[nodiscard]] ContractResult apply_settlement(
            const SettlePaymentObligationExecutionCommand& command);

        ContractStore& store_;
        AccountStore& accounts_;
        Ledger& ledger_;
        AssetId quote_asset_id_{};
    };
}  // namespace exchange
