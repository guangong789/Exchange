#pragma once

#include <map>
#include <optional>

#include "exchange/accounting/account.hpp"

namespace exchange {
    enum class ReserveResult {
        Success,
        InsufficientFunds,
    };

    enum class AvailableTransferResult {
        Success,
        InsufficientFunds,
    };

    class AccountStore {
    public:
        AccountStore() = default;
        AccountStore(const AccountStore&) = delete;
        AccountStore& operator=(const AccountStore&) = delete;
        AccountStore(AccountStore&&) = delete;
        AccountStore& operator=(AccountStore&&) = delete;

        [[nodiscard]] bool create_account(AccountId account_id);
        [[nodiscard]] bool contains_account(AccountId account_id) const noexcept;
        [[nodiscard]] std::optional<Balance> find_balance(
            AccountId account_id,
            AssetId asset_id) const;

        // Bootstrap/test funding only. This is not a general financial
        // mutation API.
        void fund(AccountId account_id, AssetId asset_id, Amount amount);

        void credit_available(
            AccountId account_id,
            AssetId asset_id,
            Amount amount);

        [[nodiscard]] AvailableTransferResult transfer_available(
            AccountId from_account_id,
            AccountId to_account_id,
            AssetId asset_id,
            Amount amount);

        [[nodiscard]] ReserveResult reserve(
            AccountId account_id,
            AssetId asset_id,
            Amount amount);

        void release(AccountId account_id, AssetId asset_id, Amount amount);

        void consume_reserved(
            AccountId account_id,
            AssetId asset_id,
            Amount amount);

    private:
        using AssetBalances = std::map<AssetId, Balance>;
        std::map<AccountId, AssetBalances> accounts_;
    };
}  // namespace exchange
