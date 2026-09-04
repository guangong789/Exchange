#include "exchange/binance_agent_os_client.hpp"

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

namespace exchange {
    TEST(BinanceAgentOsLiveTest, FetchesRealMarketSnapshot) {
        const char* access_token = std::getenv(
            "BINANCE_AGENT_OS_ACCESS_TOKEN");
        if (access_token == nullptr || access_token[0] == '\0') {
            GTEST_SKIP()
                << "BINANCE_AGENT_OS_ACCESS_TOKEN is not set; complete the "
                   "official Binance Agent OS OAuth flow first";
        }
        const char* configured_symbol = std::getenv(
            "BINANCE_AGENT_OS_SYMBOL");
        const std::string symbol =
            configured_symbol == nullptr || configured_symbol[0] == '\0'
            ? "BTCUSDT"
            : configured_symbol;

        BinanceMcpClient client;
        const ExternalMarketSnapshot snapshot =
            client.fetch_market_snapshot(symbol);

        EXPECT_EQ(snapshot.symbol, symbol);
        EXPECT_GT(snapshot.best_bid, 0);
        EXPECT_GT(snapshot.best_ask, 0);
        EXPECT_LE(snapshot.best_bid, snapshot.best_ask);
    }
}  // namespace exchange
