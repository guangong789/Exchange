#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include "agent/domain/external_market_feed.hpp"

namespace exchange {
    struct BinanceAlphaSymbolMetadata {
        std::string display_name;
        std::string alpha_id;
        std::string symbol;
        std::string stream_symbol;
        std::uint8_t price_precision{};

        bool operator==(const BinanceAlphaSymbolMetadata&) const = default;
    };

    struct BinanceAlphaTradeUpdate {
        ExternalPrice price;
        std::int64_t event_timestamp_ms{};

        bool operator==(const BinanceAlphaTradeUpdate&) const = default;
    };

    struct BinanceAlphaBookTickerUpdate {
        ExternalPrice best_bid;
        ExternalPrice best_ask;
        std::int64_t event_timestamp_ms{};

        bool operator==(const BinanceAlphaBookTickerUpdate&) const = default;
    };

    using BinanceAlphaMarketUpdate = std::variant<
        BinanceAlphaTradeUpdate,
        BinanceAlphaBookTickerUpdate>;

    class BinanceAlphaParseError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    [[nodiscard]] BinanceAlphaSymbolMetadata resolve_hakimi_alpha_symbol(
        std::string_view token_list_json,
        std::string_view exchange_info_json);

    [[nodiscard]] std::string binance_alpha_stream_path(
        const BinanceAlphaSymbolMetadata& metadata);

    [[nodiscard]] std::string binance_alpha_subscription_message(
        const BinanceAlphaSymbolMetadata& metadata);

    [[nodiscard]] BinanceAlphaMarketUpdate parse_binance_alpha_message(
        std::string_view message,
        const BinanceAlphaSymbolMetadata& metadata);

    struct BinanceAlphaMarketFeedConfig {
        std::chrono::milliseconds stale_after{5'000};
        std::chrono::milliseconds http_timeout{10'000};
        std::chrono::milliseconds reconnect_delay{1'000};
    };

    class BinanceAlphaMarketFeed final : public ExternalMarketFeed {
    public:
        explicit BinanceAlphaMarketFeed(
            BinanceAlphaMarketFeedConfig config = {});
        ~BinanceAlphaMarketFeed() override;

        BinanceAlphaMarketFeed(const BinanceAlphaMarketFeed&) = delete;
        BinanceAlphaMarketFeed& operator=(
            const BinanceAlphaMarketFeed&) = delete;

        void start();
        void stop() noexcept;

        void initialize_from_metadata(
            std::string_view token_list_json,
            std::string_view exchange_info_json);
        void ingest_message(
            std::string_view message,
            std::int64_t local_receive_timestamp_ms);

        [[nodiscard]] ExternalMarketState latest(
            std::int64_t local_now_ms) const override;
        [[nodiscard]] std::optional<BinanceAlphaSymbolMetadata> metadata()
            const;
        [[nodiscard]] std::string last_error() const;

    private:
        void run() noexcept;
        void stream_once(const BinanceAlphaSymbolMetadata& metadata);
        void set_error(std::string message);

        const BinanceAlphaMarketFeedConfig config_;
        mutable std::mutex mutex_;
        std::optional<BinanceAlphaSymbolMetadata> metadata_;
        std::optional<BinanceAlphaTradeUpdate> trade_;
        std::optional<BinanceAlphaBookTickerUpdate> book_;
        std::int64_t trade_receive_timestamp_ms_{};
        std::int64_t book_receive_timestamp_ms_{};
        std::string last_error_;
        std::atomic<bool> stop_requested_{};
        std::thread thread_;
    };
}  // namespace exchange
