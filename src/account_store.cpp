#include "exchange/account_store.hpp"

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
