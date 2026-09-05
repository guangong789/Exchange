#include "exchange/accounting/funding_coordinator.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange {
    FundingCoordinator::FundingCoordinator(
        AccountId treasury_account_id,
        AccountStore& accounts,
        Ledger& ledger)
        : treasury_account_id_(treasury_account_id),
          accounts_(accounts),
          ledger_(ledger) {
        if (treasury_account_id_ == 0) {
            throw std::invalid_argument(
                "Treasury account ID must be non-zero");
        }
    }

    FundingResult FundingCoordinator::fund(
        AccountId recipient_account_id,
        AssetId asset_id,
        Amount amount) {
        if (recipient_account_id == 0) {
            throw std::invalid_argument(
                "Funding recipient account ID must be non-zero");
        }
        if (recipient_account_id == treasury_account_id_) {
            throw std::invalid_argument(
                "Funding recipient must differ from Treasury");
        }
        if (asset_id == 0) {
            throw std::invalid_argument(
                "Funding asset ID must be non-zero");
        }
        if (amount <= 0) {
            throw std::invalid_argument(
                "Funding amount must be positive");
        }

        LedgerTransaction transaction =
            make_funding_ledger_transaction(
                treasury_account_id_,
                recipient_account_id,
                asset_id,
                amount);
        std::vector<LedgerTransaction> transactions;
        transactions.reserve(1);
        transactions.push_back(std::move(transaction));
        auto prepared = ledger_.prepare_batch(std::move(transactions));

        const AvailableTransferResult transfer_result =
            accounts_.transfer_available(
                treasury_account_id_,
                recipient_account_id,
                asset_id,
                amount);
        if (transfer_result
            == AvailableTransferResult::InsufficientFunds) {
            return FundingResult::InsufficientTreasuryFunds;
        }

        prepared.commit();
        return FundingResult::Funded;
    }
}  // namespace exchange
