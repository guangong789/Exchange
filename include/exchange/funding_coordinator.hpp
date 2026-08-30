#pragma once

#include "exchange/account_store.hpp"
#include "exchange/ledger.hpp"

namespace exchange {
    enum class FundingResult {
        Funded,
        InsufficientTreasuryFunds,
    };

    class FundingCoordinator {
    public:
        FundingCoordinator(
            AccountId treasury_account_id,
            AccountStore& accounts,
            Ledger& ledger);

        FundingCoordinator(const FundingCoordinator&) = delete;
        FundingCoordinator& operator=(const FundingCoordinator&) = delete;
        FundingCoordinator(FundingCoordinator&&) = delete;
        FundingCoordinator& operator=(FundingCoordinator&&) = delete;

        [[nodiscard]] FundingResult fund(
            AccountId recipient_account_id,
            AssetId asset_id,
            Amount amount);

    private:
        const AccountId treasury_account_id_;
        AccountStore& accounts_;
        Ledger& ledger_;
    };
}  // namespace exchange
