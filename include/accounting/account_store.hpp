#pragma once

#include <map>
#include <optional>

#include "accounting/account.hpp"

namespace exchange {
    enum class ReserveResult {
        Success,
        InsufficientFunds,
    };

    enum class AvailableTransferResult {
        Success,
        InsufficientFunds,
    };

    enum class AvailableTransferValidationResult {
        Ready,
        AccountNotFound,
        InsufficientFunds,
        DestinationOverflow,
    };

    class AccountStore {
    public:
        using AssetBalances = std::map<AssetId, Balance>;
        using AccountBalances = std::map<AccountId, AssetBalances>;

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
        [[nodiscard]] const AccountBalances& entries() const noexcept;

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

        [[nodiscard]] AvailableTransferValidationResult
        validate_available_transfer(
            AccountId from_account_id,
            AccountId to_account_id,
            AssetId asset_id,
            Amount amount) const;

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
        AccountBalances accounts_;
    };
}  // namespace exchange
