#pragma once

#include "exchange/accounting/account_store.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace exchange {
    using BootstrapFingerprint = std::array<std::uint8_t, 32>;

    struct BootstrapBalance {
        AssetId asset_id{};
        Balance balance;

        bool operator==(const BootstrapBalance&) const = default;
    };

    struct BootstrapAccount {
        AccountId account_id{};
        std::vector<BootstrapBalance> balances;

        bool operator==(const BootstrapAccount&) const = default;
    };

    struct TradingBootstrapConfig {
        std::vector<BootstrapAccount> accounts;

        bool operator==(const TradingBootstrapConfig&) const = default;
    };

    [[nodiscard]] BootstrapFingerprint calculate_bootstrap_fingerprint(
        const TradingBootstrapConfig& bootstrap);

    void apply_trading_bootstrap(
        const TradingBootstrapConfig& bootstrap,
        AccountStore& accounts);
}  // namespace exchange
