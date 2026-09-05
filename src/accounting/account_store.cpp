#include "exchange/accounting/account_store.hpp"

#include <limits>
#include <stdexcept>

namespace exchange {
    namespace {
        void validate_balance_mutation(
            AccountId account_id,
            AssetId asset_id,
            Amount amount) {
            if (account_id == 0) {
                throw std::invalid_argument("account ID must be non-zero");
            }
            if (asset_id == 0) {
                throw std::invalid_argument("asset ID must be non-zero");
            }
            if (amount <= 0) {
                throw std::invalid_argument("amount must be positive");
            }
        }
    }  // namespace

    bool AccountStore::create_account(AccountId account_id) {
        if (account_id == 0) {
            throw std::invalid_argument("account ID must be non-zero");
        }

        return accounts_.try_emplace(account_id).second;
    }

    bool AccountStore::contains_account(AccountId account_id) const noexcept {
        return account_id != 0 && accounts_.contains(account_id);
    }

    std::optional<Balance> AccountStore::find_balance(
        AccountId account_id,
        AssetId asset_id) const {
        if (account_id == 0) {
            throw std::invalid_argument("account ID must be non-zero");
        }
        if (asset_id == 0) {
            throw std::invalid_argument("asset ID must be non-zero");
        }

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            return std::nullopt;
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()) {
            return std::nullopt;
        }

        return balance->second;
    }

    void AccountStore::fund(
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        if (account_id == 0) {
            throw std::invalid_argument("account ID must be non-zero");
        }
        if (asset_id == 0) {
            throw std::invalid_argument("asset ID must be non-zero");
        }
        if (amount <= 0) {
            throw std::invalid_argument("funding amount must be positive");
        }

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            throw std::out_of_range("account does not exist");
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()) {
            account->second.emplace(asset_id, Balance{amount, 0});
            return;
        }

        if (amount > std::numeric_limits<Amount>::max() - balance->second.available) {
            throw std::overflow_error("available balance overflow");
        }

        balance->second.available += amount;
    }

    void AccountStore::credit_available(
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_balance_mutation(account_id, asset_id, amount);

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            throw std::out_of_range("account does not exist");
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()) {
            account->second.emplace(asset_id, Balance{amount, 0});
            return;
        }

        if (amount
            > std::numeric_limits<Amount>::max()
                  - balance->second.available) {
            throw std::overflow_error("available balance overflow");
        }

        balance->second.available += amount;
    }

    AvailableTransferResult AccountStore::transfer_available(
        AccountId from_account_id,
        AccountId to_account_id,
        AssetId asset_id,
        Amount amount) {
        validate_balance_mutation(from_account_id, asset_id, amount);
        validate_balance_mutation(to_account_id, asset_id, amount);
        if (from_account_id == to_account_id) {
            throw std::invalid_argument(
                "available transfer accounts must be distinct");
        }

        const auto from_account = accounts_.find(from_account_id);
        if (from_account == accounts_.end()) {
            throw std::out_of_range("source account does not exist");
        }
        const auto to_account = accounts_.find(to_account_id);
        if (to_account == accounts_.end()) {
            throw std::out_of_range("destination account does not exist");
        }

        const auto from_balance = from_account->second.find(asset_id);
        if (from_balance == from_account->second.end()
            || from_balance->second.available < amount) {
            return AvailableTransferResult::InsufficientFunds;
        }

        auto to_balance = to_account->second.find(asset_id);
        if (to_balance != to_account->second.end()
            && to_balance->second.available
                   > std::numeric_limits<Amount>::max() - amount) {
            throw std::overflow_error(
                "destination available balance overflow");
        }

        if (to_balance == to_account->second.end()) {
            to_balance = to_account->second
                             .emplace(asset_id, Balance{})
                             .first;
        }

        from_balance->second.available -= amount;
        to_balance->second.available += amount;
        return AvailableTransferResult::Success;
    }

    ReserveResult AccountStore::reserve(
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_balance_mutation(account_id, asset_id, amount);

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            throw std::out_of_range("account does not exist");
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()
            || balance->second.available < amount) {
            return ReserveResult::InsufficientFunds;
        }

        if (amount
            > std::numeric_limits<Amount>::max() - balance->second.reserved) {
            throw std::overflow_error("reserved balance overflow");
        }

        balance->second.available -= amount;
        balance->second.reserved += amount;
        return ReserveResult::Success;
    }

    void AccountStore::release(
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_balance_mutation(account_id, asset_id, amount);

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            throw std::out_of_range("account does not exist");
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()) {
            throw std::logic_error("asset balance does not exist");
        }
        if (amount > balance->second.reserved) {
            throw std::logic_error("release exceeds reserved balance");
        }
        if (amount
            > std::numeric_limits<Amount>::max() - balance->second.available) {
            throw std::overflow_error("available balance overflow");
        }

        balance->second.reserved -= amount;
        balance->second.available += amount;
    }

    void AccountStore::consume_reserved(
        AccountId account_id,
        AssetId asset_id,
        Amount amount) {
        validate_balance_mutation(account_id, asset_id, amount);

        const auto account = accounts_.find(account_id);
        if (account == accounts_.end()) {
            throw std::out_of_range("account does not exist");
        }

        const auto balance = account->second.find(asset_id);
        if (balance == account->second.end()) {
            throw std::logic_error("asset balance does not exist");
        }
        if (amount > balance->second.reserved) {
            throw std::logic_error("consumption exceeds reserved balance");
        }

        balance->second.reserved -= amount;
    }
}  // namespace exchange
