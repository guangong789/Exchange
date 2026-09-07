#include "exchange/execution/trading_bootstrap.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        TradingBootstrapConfig reference_bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{10, {1'000, 25}}, {20, {2'000, 0}}}},
                BootstrapAccount{2, {{10, {500, 10}}}},
                BootstrapAccount{3, {}},
            }};
        }

        TEST(TradingBootstrapTest, IdenticalConfigurationHasSameFingerprint) {
            EXPECT_EQ(
                calculate_bootstrap_fingerprint(reference_bootstrap()),
                calculate_bootstrap_fingerprint(reference_bootstrap()));
        }

        TEST(TradingBootstrapTest, UsesStableCanonicalSha256Encoding) {
            constexpr BootstrapFingerprint expected{
                0xca, 0xb8, 0xde, 0x41, 0xc1, 0x04, 0x66, 0xdd,
                0x87, 0x5c, 0x91, 0x67, 0x2a, 0x75, 0xc2, 0xae,
                0x62, 0x7c, 0x71, 0x41, 0xa2, 0xc0, 0x6a, 0x67,
                0x2d, 0xf9, 0x96, 0xa7, 0x76, 0x6f, 0x75, 0xe0};

            EXPECT_EQ(
                calculate_bootstrap_fingerprint(reference_bootstrap()),
                expected);
        }

        TEST(TradingBootstrapTest, AccountAndAssetOrderDoNotAffectFingerprint) {
            const TradingBootstrapConfig reordered{{
                BootstrapAccount{3, {}},
                BootstrapAccount{2, {{10, {500, 10}}}},
                BootstrapAccount{
                    1,
                    {{20, {2'000, 0}}, {10, {1'000, 25}}}},
            }};

            EXPECT_EQ(
                calculate_bootstrap_fingerprint(reference_bootstrap()),
                calculate_bootstrap_fingerprint(reordered));
        }

        TEST(TradingBootstrapTest, SemanticChangesAlterFingerprint) {
            const BootstrapFingerprint reference =
                calculate_bootstrap_fingerprint(reference_bootstrap());

            TradingBootstrapConfig changed_account = reference_bootstrap();
            changed_account.accounts[0].account_id = 4;
            EXPECT_NE(
                calculate_bootstrap_fingerprint(changed_account), reference);

            TradingBootstrapConfig removed_account = reference_bootstrap();
            removed_account.accounts.pop_back();
            EXPECT_NE(
                calculate_bootstrap_fingerprint(removed_account), reference);

            TradingBootstrapConfig extra_account = reference_bootstrap();
            extra_account.accounts.push_back(BootstrapAccount{4, {}});
            EXPECT_NE(
                calculate_bootstrap_fingerprint(extra_account), reference);

            TradingBootstrapConfig changed_asset = reference_bootstrap();
            changed_asset.accounts[0].balances[0].asset_id = 11;
            EXPECT_NE(
                calculate_bootstrap_fingerprint(changed_asset), reference);

            TradingBootstrapConfig changed_balance = reference_bootstrap();
            ++changed_balance.accounts[0].balances[0].balance.available;
            EXPECT_NE(
                calculate_bootstrap_fingerprint(changed_balance), reference);

            TradingBootstrapConfig changed_reservation = reference_bootstrap();
            ++changed_reservation.accounts[0].balances[0].balance.reserved;
            EXPECT_NE(
                calculate_bootstrap_fingerprint(changed_reservation),
                reference);
        }

        TEST(TradingBootstrapTest, AppliesExactAvailableAndReservedState) {
            AccountStore accounts;

            apply_trading_bootstrap(reference_bootstrap(), accounts);

            EXPECT_TRUE(accounts.contains_account(3));
            EXPECT_EQ(accounts.find_balance(1, 10), (Balance{1'000, 25}));
            EXPECT_EQ(accounts.find_balance(1, 20), (Balance{2'000, 0}));
            EXPECT_EQ(accounts.find_balance(2, 10), (Balance{500, 10}));
        }

        TEST(TradingBootstrapTest, RejectsAmbiguousOrInvalidRows) {
            EXPECT_THROW(
                static_cast<void>(calculate_bootstrap_fingerprint(
                    TradingBootstrapConfig{{BootstrapAccount{1, {}},
                                            BootstrapAccount{1, {}}}})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_bootstrap_fingerprint(
                    TradingBootstrapConfig{{BootstrapAccount{
                        1, {{10, {1, 0}}, {10, {2, 0}}}}}})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_bootstrap_fingerprint(
                    TradingBootstrapConfig{{BootstrapAccount{
                        1, {{10, {0, 0}}}}}})),
                std::invalid_argument);
            EXPECT_THROW(
                static_cast<void>(calculate_bootstrap_fingerprint(
                    TradingBootstrapConfig{{BootstrapAccount{
                        1,
                        {{10,
                          {std::numeric_limits<Amount>::max(), 1}}}}}})),
                std::invalid_argument);
        }
    }  // namespace
}  // namespace exchange
