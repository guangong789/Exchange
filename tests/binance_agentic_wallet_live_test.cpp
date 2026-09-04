#include "exchange/binance_agentic_wallet.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

namespace exchange {
    TEST(BinanceAgenticWalletLiveTest, PreviewsX402Requirement) {
        const char* configured_cli = std::getenv("EXCHANGE_BINANCE_WALLET_CLI");
        BinanceAgenticWalletCliBridge bridge(
            BinanceAgenticWalletCliConfig{
                configured_cli == nullptr ? "baw" : configured_cli,
                std::chrono::seconds(30),
                BinanceX402AcceptExtra{"Tether USD", "1"}});

        // Current Binance Agentic Wallet documentation supports x402 V2 on
        // BSC mainnet (eip155:56). This is preview-only; no sign call follows.
        const ExternalPaymentRequirement requirement{
            2,
            "http://127.0.0.1/premium-signal",
            "Exchange preview-only live smoke test",
            "application/json",
            "exact",
            "eip155:56",
            "10000",
            "0x55d398326f99059fF775485246999027B3197955",
            "0x1111111111111111111111111111111111111111",
            60,
        };

        try {
            const ExternalPaymentPreview preview = bridge.preview(requirement);
            std::cout
                << "Binance wallet preview:\n"
                << "provider=" << preview.provider << '\n'
                << "network=" << preview.network << '\n'
                << "asset=" << preview.asset << '\n'
                << "amount=" << preview.amount << '\n'
                << "status=" << preview.provider_status << '\n'
                << "reasons=[";
            for (std::size_t index = 0; index < preview.reasons.size(); ++index) {
                if (index != 0) {
                    std::cout << ", ";
                }
                std::cout << preview.reasons[index];
            }
            std::cout << "]\n"
                      << "(preview only; not signed or settled)\n";
            EXPECT_FALSE(preview.provider.empty());
            EXPECT_FALSE(preview.provider_status.empty());
        } catch (const BinanceAgenticWalletNoSessionError& error) {
            GTEST_SKIP() << error.what();
        }
    }
}  // namespace exchange
