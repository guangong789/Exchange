#include "agent/provider/binance_alpha/binance_alpha_market_feed.hpp"

#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        constexpr std::string_view token_metadata = R"({
          "code":"000000","message":"","success":true,"data":[
            {"name":"other","symbol":"OTHER","alphaId":"ALPHA_1","offline":false},
            {"name":"哈基米","symbol":"哈基米","alphaId":"ALPHA_426",
             "chainId":"56","chainName":"BSC",
             "contractAddress":"0x82ec31d69b3c289e541b50e30681fd1acad24444",
             "offline":false,"listingTime":1760369400000}
          ]
        })";

        constexpr std::string_view exchange_metadata = R"({
          "code":"000000","message":"","success":true,"data":{
            "timezone":"UTC","symbols":[
              {"symbol":"ALPHA_426USDC","status":"TRADING",
               "baseAsset":"ALPHA_426","quoteAsset":"USDC",
               "pricePrecision":8,"quantityPrecision":8},
              {"symbol":"ALPHA_426USDT","status":"TRADING",
               "baseAsset":"ALPHA_426","quoteAsset":"USDT",
               "pricePrecision":8,"quantityPrecision":8},
              {"symbol":"ALPHA_426U","status":"DELISTED",
               "baseAsset":"ALPHA_426","quoteAsset":"U",
               "pricePrecision":8,"quantityPrecision":8}
            ]
          }
        })";

        const BinanceAlphaSymbolMetadata hakimi_metadata =
            resolve_hakimi_alpha_symbol(token_metadata, exchange_metadata);

        TEST(BinanceAlphaMetadataTest,
             ResolvesOnlyCurrentHakimiUsdtSymbolFromOfficialShape) {
            EXPECT_EQ(
                hakimi_metadata,
                (BinanceAlphaSymbolMetadata{
                    "哈基米",
                    "ALPHA_426",
                    "ALPHA_426USDT",
                    "alpha_426usdt",
                    8}));
            EXPECT_EQ(
                binance_alpha_stream_path(hakimi_metadata),
                "/w3w/wsa/stream");
            EXPECT_EQ(
                binance_alpha_subscription_message(hakimi_metadata),
                R"({"id":1,"method":"SUBSCRIBE","params":["alpha_426usdt@aggTrade","alpha_426usdt@bookTicker"]})");
        }

        TEST(BinanceAlphaMetadataTest,
             RejectsMissingOfflineAndAmbiguousHakimiMetadata) {
            EXPECT_THROW(
                static_cast<void>(resolve_hakimi_alpha_symbol(
                    R"({"code":"000000","success":true,"data":[]})",
                    exchange_metadata)),
                BinanceAlphaParseError);
            EXPECT_THROW(
                static_cast<void>(resolve_hakimi_alpha_symbol(
                    R"({"code":"000000","success":true,"data":[{"name":"哈基米","symbol":"哈基米","alphaId":"ALPHA_426","offline":true}]})",
                    exchange_metadata)),
                BinanceAlphaParseError);
            EXPECT_THROW(
                static_cast<void>(resolve_hakimi_alpha_symbol(
                    R"({"code":"000000","success":true,"data":[{"name":"哈基米","symbol":"哈基米","alphaId":"ALPHA_426","offline":false},{"name":"哈基米","symbol":"哈基米","alphaId":"ALPHA_999","offline":false}]})",
                    exchange_metadata)),
                BinanceAlphaParseError);
        }

        TEST(BinanceAlphaMessageParserTest,
             ParsesCombinedTradeAndBookTickerMessagesExactly) {
            const auto trade = parse_binance_alpha_message(
                R"({"stream":"alpha_426usdt@aggTrade","data":{"e":"aggTrade","E":1773110023891,"T":1773110023877,"a":10,"f":10,"l":10,"m":false,"p":"0.04072338","q":"879.24000000","s":"ALPHA_426USDT"}})",
                hakimi_metadata);
            ASSERT_TRUE(std::holds_alternative<BinanceAlphaTradeUpdate>(trade));
            EXPECT_EQ(
                std::get<BinanceAlphaTradeUpdate>(trade),
                (BinanceAlphaTradeUpdate{
                    ExternalPrice{4'072'338, 8},
                    1'773'110'023'891}));

            const auto book = parse_binance_alpha_message(
                R"({"stream":"alpha_426usdt@bookTicker","data":{"e":"bookTicker","E":1773110023900,"T":1773110023899,"u":11,"s":"ALPHA_426USDT","b":"0.04070000","B":"10.0","a":"0.04080000","A":"11.0"}})",
                hakimi_metadata);
            ASSERT_TRUE(
                std::holds_alternative<BinanceAlphaBookTickerUpdate>(book));
            EXPECT_EQ(
                std::get<BinanceAlphaBookTickerUpdate>(book),
                (BinanceAlphaBookTickerUpdate{
                    ExternalPrice{4'070'000, 8},
                    ExternalPrice{4'080'000, 8},
                    1'773'110'023'900}));
        }

        TEST(BinanceAlphaMessageParserTest,
             RejectsMalformedWrongSymbolAndInvalidPrices) {
            for (const std::string& message : {
                     "not-json",
                     R"({"e":"aggTrade","E":1,"s":"ALPHA_999USDT","p":"0.04"})",
                     R"({"e":"aggTrade","E":1,"s":"ALPHA_426USDT","p":"0"})",
                     R"({"e":"bookTicker","E":1,"s":"ALPHA_426USDT","b":"0.05","a":"0.04"})",
                     R"({"e":"depthUpdate","E":1,"s":"ALPHA_426USDT"})"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_binance_alpha_message(
                        message,
                        hakimi_metadata)),
                    BinanceAlphaParseError);
            }
        }

        TEST(BinanceAlphaMarketFeedTest,
             PublishesCompleteCopyableSnapshotAndMarksItStale) {
            BinanceAlphaMarketFeed feed(
                BinanceAlphaMarketFeedConfig{
                    std::chrono::milliseconds{100},
                    std::chrono::milliseconds{1'000},
                    std::chrono::milliseconds{10}});
            feed.initialize_from_metadata(token_metadata, exchange_metadata);

            EXPECT_EQ(
                feed.latest(900).freshness,
                ExternalMarketFreshness::Unavailable);
            feed.ingest_message(
                R"({"e":"aggTrade","E":1000,"s":"ALPHA_426USDT","p":"0.04072338"})",
                1'000);
            EXPECT_EQ(
                feed.latest(1'000).freshness,
                ExternalMarketFreshness::Unavailable);
            feed.ingest_message(
                R"({"e":"bookTicker","E":1001,"s":"ALPHA_426USDT","b":"0.04070000","a":"0.04080000"})",
                1'001);

            const ExternalMarketState fresh = feed.latest(1'100);
            EXPECT_EQ(fresh.source, ExternalMarketSource::BinanceAlpha);
            EXPECT_EQ(fresh.symbol, "ALPHA_426USDT");
            EXPECT_EQ(
                fresh.latest_trade_price,
                (ExternalPrice{4'072'338, 8}));
            EXPECT_EQ(fresh.best_bid, (ExternalPrice{4'070'000, 8}));
            EXPECT_EQ(fresh.best_ask, (ExternalPrice{4'080'000, 8}));
            EXPECT_EQ(fresh.event_timestamp_ms, 1'000);
            EXPECT_EQ(fresh.local_receive_timestamp_ms, 1'000);
            EXPECT_EQ(fresh.freshness, ExternalMarketFreshness::Fresh);

            ExternalMarketState copied = fresh;
            copied.symbol = "changed";
            EXPECT_EQ(feed.latest(1'100).symbol, "ALPHA_426USDT");
            EXPECT_EQ(
                feed.latest(1'101).freshness,
                ExternalMarketFreshness::Stale);
        }
    }  // namespace
}  // namespace exchange
