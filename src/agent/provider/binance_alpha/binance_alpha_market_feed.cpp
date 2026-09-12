#include "agent/provider/binance_alpha/binance_alpha_market_feed.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

namespace exchange {
    namespace {
        using Json = nlohmann::json;
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        namespace ssl = asio::ssl;
        namespace websocket = beast::websocket;
        using Tcp = asio::ip::tcp;

        constexpr std::string_view kHakimiDisplayName = "哈基米";
        constexpr std::string_view kQuoteAsset = "USDT";
        constexpr std::string_view kTokenListUrl =
            "https://www.binance.com/bapi/defi/v1/public/wallet-direct/"
            "buw/wallet/cex/alpha/all/token/list";
        constexpr std::string_view kExchangeInfoUrl =
            "https://www.binance.com/bapi/defi/v1/public/alpha-trade/"
            "get-exchange-info";
        constexpr std::string_view kWebSocketHost = "nbstream.binance.com";
        constexpr std::string_view kWebSocketPort = "443";
        constexpr std::size_t kMaximumHttpResponseSize = 16 * 1024 * 1024;

        class CurlGlobalState {
        public:
            CurlGlobalState() {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                    throw std::runtime_error(
                        "Failed to initialize Binance Alpha HTTP transport");
                }
            }

            ~CurlGlobalState() {
                curl_global_cleanup();
            }
        };

        CurlGlobalState& curl_global_state() {
            static CurlGlobalState state;
            return state;
        }

        struct HttpResponseBuffer {
            std::string content;
            bool exceeded_limit{};
        };

        std::size_t append_http_response(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > kMaximumHttpResponseSize / size) {
                static_cast<HttpResponseBuffer*>(context)->exceeded_limit =
                    true;
                return 0;
            }
            const std::size_t byte_count = size * count;
            auto& response = *static_cast<HttpResponseBuffer*>(context);
            if (byte_count
                > kMaximumHttpResponseSize - response.content.size()) {
                response.exceeded_limit = true;
                return 0;
            }
            try {
                response.content.append(data, byte_count);
                return byte_count;
            } catch (...) {
                return 0;
            }
        }

        std::string public_http_get(
            std::string_view url,
            std::chrono::milliseconds timeout) {
            static_cast<void>(curl_global_state());
            using CurlHandle =
                std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
            CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
            if (!handle) {
                throw std::runtime_error(
                    "Failed to create Binance Alpha metadata request");
            }

            const std::string owned_url{url};
            HttpResponseBuffer response;
            char error_buffer[CURL_ERROR_SIZE]{};
            const auto set_option = [&handle](CURLoption option, auto value) {
                if (curl_easy_setopt(handle.get(), option, value)
                    != CURLE_OK) {
                    throw std::runtime_error(
                        "Failed to configure Binance Alpha metadata request");
                }
            };
            set_option(CURLOPT_URL, owned_url.c_str());
            set_option(CURLOPT_WRITEFUNCTION, &append_http_response);
            set_option(CURLOPT_WRITEDATA, &response);
            set_option(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
            set_option(
                CURLOPT_CONNECTTIMEOUT_MS,
                static_cast<long>(timeout.count()));
            set_option(CURLOPT_NOSIGNAL, 1L);
            set_option(CURLOPT_FOLLOWLOCATION, 1L);
            set_option(CURLOPT_MAXREDIRS, 3L);
            set_option(CURLOPT_ERRORBUFFER, error_buffer);

            const CURLcode result = curl_easy_perform(handle.get());
            if (result != CURLE_OK) {
                if (response.exceeded_limit) {
                    throw std::runtime_error(
                        "Binance Alpha metadata response exceeded size limit");
                }
                const char* detail = error_buffer[0] == '\0'
                    ? curl_easy_strerror(result)
                    : error_buffer;
                throw std::runtime_error(
                    std::string("Binance Alpha metadata request failed: ")
                    + detail);
            }

            long status = 0;
            if (curl_easy_getinfo(
                    handle.get(),
                    CURLINFO_RESPONSE_CODE,
                    &status)
                != CURLE_OK) {
                throw std::runtime_error(
                    "Failed to read Binance Alpha metadata HTTP status");
            }
            if (status < 200 || status >= 300) {
                throw std::runtime_error(
                    "Binance Alpha metadata returned HTTP status "
                    + std::to_string(status));
            }
            return response.content;
        }

        const Json& require_object_field(
            const Json& object,
            const char* field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_object()) {
                throw BinanceAlphaParseError(
                    "Binance Alpha response is missing an object field");
            }
            return *value;
        }

        const Json& require_array_field(
            const Json& object,
            const char* field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_array()) {
                throw BinanceAlphaParseError(
                    "Binance Alpha response is missing an array field");
            }
            return *value;
        }

        std::string require_string(const Json& object, const char* field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_string()) {
                throw BinanceAlphaParseError(
                    "Binance Alpha response is missing a string field");
            }
            return value->get<std::string>();
        }

        std::int64_t require_timestamp(
            const Json& object,
            const char* field) {
            const auto value = object.find(field);
            if (value == object.end()
                || (!value->is_number_integer()
                    && !value->is_number_unsigned())) {
                throw BinanceAlphaParseError(
                    "Binance Alpha response has an invalid timestamp");
            }
            if (value->is_number_unsigned()) {
                const auto number = value->get<std::uint64_t>();
                if (number
                    > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                    throw BinanceAlphaParseError(
                        "Binance Alpha timestamp is out of range");
                }
                return static_cast<std::int64_t>(number);
            }
            const auto number = value->get<std::int64_t>();
            if (number < 0) {
                throw BinanceAlphaParseError(
                    "Binance Alpha timestamp must be non-negative");
            }
            return number;
        }

        ExternalPrice parse_price(
            std::string_view text,
            std::uint8_t precision) {
            if (text.empty() || precision > 18) {
                throw BinanceAlphaParseError(
                    "Binance Alpha price has invalid precision");
            }
            const std::size_t decimal = text.find('.');
            if (decimal != std::string_view::npos
                && text.find('.', decimal + 1) != std::string_view::npos) {
                throw BinanceAlphaParseError(
                    "Binance Alpha price has multiple decimal points");
            }
            const std::string_view whole = decimal == std::string_view::npos
                ? text
                : text.substr(0, decimal);
            const std::string_view fraction = decimal == std::string_view::npos
                ? std::string_view{}
                : text.substr(decimal + 1);
            if (whole.empty() || fraction.size() > precision) {
                throw BinanceAlphaParseError(
                    "Binance Alpha price does not match metadata precision");
            }

            std::uint64_t units = 0;
            const auto append_digit = [&](char character) {
                if (!std::isdigit(static_cast<unsigned char>(character))) {
                    throw BinanceAlphaParseError(
                        "Binance Alpha price is not a decimal number");
                }
                const std::uint64_t digit =
                    static_cast<std::uint64_t>(character - '0');
                if (units
                    > (static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())
                       - digit)
                          / 10) {
                    throw BinanceAlphaParseError(
                        "Binance Alpha price is out of range");
                }
                units = units * 10 + digit;
            };
            for (const char character : whole) {
                append_digit(character);
            }
            for (const char character : fraction) {
                append_digit(character);
            }
            for (std::size_t index = fraction.size(); index < precision;
                 ++index) {
                append_digit('0');
            }
            if (units == 0) {
                throw BinanceAlphaParseError(
                    "Binance Alpha price must be positive");
            }
            return ExternalPrice{
                static_cast<std::int64_t>(units),
                precision};
        }

        std::string lowercase_ascii(std::string value) {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        }

        std::int64_t system_time_millis() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        bool is_subscription_acknowledgement(
            std::string_view message) {
            try {
                const Json value = Json::parse(message);
                return value.is_object()
                    && value.value("id", std::uint64_t{}) == 1
                    && value.contains("result")
                    && value["result"].is_null()
                    && !value.contains("error");
            } catch (const nlohmann::json::exception&) {
                return false;
            }
        }
    }  // namespace

    BinanceAlphaSymbolMetadata resolve_hakimi_alpha_symbol(
        std::string_view token_list_json,
        std::string_view exchange_info_json) {
        try {
            const Json tokens = Json::parse(token_list_json);
            if (tokens.value("success", false) != true
                || tokens.value("code", std::string{}) != "000000") {
                throw BinanceAlphaParseError(
                    "Binance Alpha token metadata request was unsuccessful");
            }
            const Json& token_data = require_array_field(tokens, "data");
            const Json* hakimi = nullptr;
            for (const Json& token : token_data) {
                if (!token.is_object()) {
                    continue;
                }
                if (token.value("name", std::string{}) == kHakimiDisplayName
                    && token.value("symbol", std::string{})
                           == kHakimiDisplayName
                    && !token.value("offline", true)) {
                    if (hakimi != nullptr) {
                        throw BinanceAlphaParseError(
                            "Binance Alpha metadata has multiple active 哈基米 tokens");
                    }
                    hakimi = &token;
                }
            }
            if (hakimi == nullptr) {
                throw BinanceAlphaParseError(
                    "Binance Alpha metadata has no active 哈基米 token");
            }
            const std::string alpha_id = require_string(*hakimi, "alphaId");
            if (!alpha_id.starts_with("ALPHA_")) {
                throw BinanceAlphaParseError(
                    "Binance Alpha 哈基米 token has an invalid alphaId");
            }

            const Json exchange_info = Json::parse(exchange_info_json);
            if (exchange_info.value("success", false) != true
                || exchange_info.value("code", std::string{}) != "000000") {
                throw BinanceAlphaParseError(
                    "Binance Alpha exchange metadata request was unsuccessful");
            }
            const Json& exchange_data =
                require_object_field(exchange_info, "data");
            const Json& symbols =
                require_array_field(exchange_data, "symbols");
            const Json* selected = nullptr;
            for (const Json& symbol : symbols) {
                if (!symbol.is_object()) {
                    continue;
                }
                if (symbol.value("baseAsset", std::string{}) == alpha_id
                    && symbol.value("quoteAsset", std::string{})
                           == kQuoteAsset
                    && symbol.value("status", std::string{}) == "TRADING") {
                    if (selected != nullptr) {
                        throw BinanceAlphaParseError(
                            "Binance Alpha metadata has multiple active 哈基米 USDT symbols");
                    }
                    selected = &symbol;
                }
            }
            if (selected == nullptr) {
                throw BinanceAlphaParseError(
                    "Binance Alpha metadata has no active 哈基米 USDT symbol");
            }

            const auto precision_field = selected->find("pricePrecision");
            if (precision_field == selected->end()
                || !precision_field->is_number_unsigned()) {
                throw BinanceAlphaParseError(
                    "Binance Alpha symbol has no price precision");
            }
            const auto precision = precision_field->get<std::uint64_t>();
            if (precision > 18) {
                throw BinanceAlphaParseError(
                    "Binance Alpha price precision is unsupported");
            }
            const std::string symbol = require_string(*selected, "symbol");
            return BinanceAlphaSymbolMetadata{
                std::string{kHakimiDisplayName},
                alpha_id,
                symbol,
                lowercase_ascii(symbol),
                static_cast<std::uint8_t>(precision),
            };
        } catch (const BinanceAlphaParseError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw BinanceAlphaParseError(
                "Binance Alpha metadata is not valid JSON");
        }
    }

    std::string binance_alpha_stream_path(
        const BinanceAlphaSymbolMetadata& metadata) {
        if (metadata.stream_symbol.empty()) {
            throw std::invalid_argument(
                "Binance Alpha stream symbol must be non-empty");
        }
        return "/w3w/wsa/stream";
    }

    std::string binance_alpha_subscription_message(
        const BinanceAlphaSymbolMetadata& metadata) {
        if (metadata.stream_symbol.empty()) {
            throw std::invalid_argument(
                "Binance Alpha stream symbol must be non-empty");
        }
        return Json{
            {"method", "SUBSCRIBE"},
            {"params",
             Json::array({
                 metadata.stream_symbol + "@aggTrade",
                 metadata.stream_symbol + "@bookTicker"})},
            {"id", 1}}
            .dump();
    }

    BinanceAlphaMarketUpdate parse_binance_alpha_message(
        std::string_view message,
        const BinanceAlphaSymbolMetadata& metadata) {
        try {
            const Json envelope = Json::parse(message);
            if (!envelope.is_object()) {
                throw BinanceAlphaParseError(
                    "Binance Alpha message must be a JSON object");
            }
            const Json* payload = &envelope;
            const auto data = envelope.find("data");
            if (data != envelope.end()) {
                if (!data->is_object()) {
                    throw BinanceAlphaParseError(
                        "Binance Alpha combined message has invalid data");
                }
                payload = &*data;
            }
            if (require_string(*payload, "s") != metadata.symbol) {
                throw BinanceAlphaParseError(
                    "Binance Alpha message has an unexpected symbol");
            }
            const std::string event = require_string(*payload, "e");
            const std::int64_t event_timestamp =
                require_timestamp(*payload, "E");
            if (event == "aggTrade") {
                return BinanceAlphaTradeUpdate{
                    parse_price(
                        require_string(*payload, "p"),
                        metadata.price_precision),
                    event_timestamp,
                };
            }
            if (event == "bookTicker") {
                const ExternalPrice bid = parse_price(
                    require_string(*payload, "b"),
                    metadata.price_precision);
                const ExternalPrice ask = parse_price(
                    require_string(*payload, "a"),
                    metadata.price_precision);
                if (bid.units > ask.units) {
                    throw BinanceAlphaParseError(
                        "Binance Alpha best bid exceeds best ask");
                }
                return BinanceAlphaBookTickerUpdate{
                    bid,
                    ask,
                    event_timestamp,
                };
            }
            throw BinanceAlphaParseError(
                "Binance Alpha message has an unsupported event type");
        } catch (const BinanceAlphaParseError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw BinanceAlphaParseError(
                "Binance Alpha market message is not valid JSON");
        }
    }

    BinanceAlphaMarketFeed::BinanceAlphaMarketFeed(
        BinanceAlphaMarketFeedConfig config)
        : config_(config) {
        if (config_.stale_after.count() <= 0
            || config_.http_timeout.count() <= 0
            || config_.http_timeout.count() > LONG_MAX
            || config_.reconnect_delay.count() <= 0) {
            throw std::invalid_argument(
                "Binance Alpha feed durations must be positive and in range");
        }
    }

    BinanceAlphaMarketFeed::~BinanceAlphaMarketFeed() {
        stop();
    }

    void BinanceAlphaMarketFeed::start() {
        if (thread_.joinable()) {
            throw std::logic_error("Binance Alpha feed is already running");
        }
        stop_requested_ = false;
        thread_ = std::thread([this] { run(); });
    }

    void BinanceAlphaMarketFeed::stop() noexcept {
        stop_requested_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void BinanceAlphaMarketFeed::initialize_from_metadata(
        std::string_view token_list_json,
        std::string_view exchange_info_json) {
        BinanceAlphaSymbolMetadata resolved = resolve_hakimi_alpha_symbol(
            token_list_json,
            exchange_info_json);
        std::lock_guard lock{mutex_};
        metadata_ = std::move(resolved);
        trade_.reset();
        book_.reset();
        trade_receive_timestamp_ms_ = 0;
        book_receive_timestamp_ms_ = 0;
        last_error_.clear();
    }

    void BinanceAlphaMarketFeed::ingest_message(
        std::string_view message,
        std::int64_t local_receive_timestamp_ms) {
        if (local_receive_timestamp_ms < 0) {
            throw std::invalid_argument(
                "Binance Alpha receive timestamp must be non-negative");
        }
        std::lock_guard lock{mutex_};
        if (!metadata_.has_value()) {
            throw std::logic_error(
                "Binance Alpha metadata is not initialized");
        }
        const BinanceAlphaMarketUpdate update =
            parse_binance_alpha_message(message, *metadata_);
        std::visit(
            [&](const auto& value) {
                using Update = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<
                                  Update,
                                  BinanceAlphaTradeUpdate>) {
                    if (!trade_.has_value()
                        || value.event_timestamp_ms
                               >= trade_->event_timestamp_ms) {
                        trade_ = value;
                        trade_receive_timestamp_ms_ =
                            local_receive_timestamp_ms;
                    }
                } else {
                    if (!book_.has_value()
                        || value.event_timestamp_ms
                               >= book_->event_timestamp_ms) {
                        book_ = value;
                        book_receive_timestamp_ms_ =
                            local_receive_timestamp_ms;
                    }
                }
            },
            update);
    }

    ExternalMarketState BinanceAlphaMarketFeed::latest(
        std::int64_t local_now_ms) const {
        if (local_now_ms < 0) {
            throw std::invalid_argument(
                "Binance Alpha current timestamp must be non-negative");
        }
        std::lock_guard lock{mutex_};
        ExternalMarketState state;
        if (!metadata_.has_value()) {
            return state;
        }
        state.symbol = metadata_->symbol;
        if (!trade_.has_value() || !book_.has_value()) {
            return state;
        }

        state.latest_trade_price = trade_->price;
        state.best_bid = book_->best_bid;
        state.best_ask = book_->best_ask;
        state.event_timestamp_ms = std::min(
            trade_->event_timestamp_ms,
            book_->event_timestamp_ms);
        state.local_receive_timestamp_ms = std::min(
            trade_receive_timestamp_ms_,
            book_receive_timestamp_ms_);
        if (local_now_ms < state.local_receive_timestamp_ms
            || local_now_ms - state.local_receive_timestamp_ms
                   > config_.stale_after.count()) {
            state.freshness = ExternalMarketFreshness::Stale;
        } else {
            state.freshness = ExternalMarketFreshness::Fresh;
        }
        return state;
    }

    std::optional<BinanceAlphaSymbolMetadata>
        BinanceAlphaMarketFeed::metadata() const {
        std::lock_guard lock{mutex_};
        return metadata_;
    }

    std::string BinanceAlphaMarketFeed::last_error() const {
        std::lock_guard lock{mutex_};
        return last_error_;
    }

    void BinanceAlphaMarketFeed::run() noexcept {
        try {
            initialize_from_metadata(
                public_http_get(kTokenListUrl, config_.http_timeout),
                public_http_get(kExchangeInfoUrl, config_.http_timeout));
        } catch (const std::exception& error) {
            set_error(error.what());
            return;
        }

        while (!stop_requested_) {
            try {
                stream_once(*metadata());
            } catch (const std::exception& error) {
                set_error(error.what());
            }
            if (!stop_requested_) {
                std::this_thread::sleep_for(config_.reconnect_delay);
            }
        }
    }

    void BinanceAlphaMarketFeed::stream_once(
        const BinanceAlphaSymbolMetadata& metadata) {
        asio::io_context context;
        ssl::context tls{ssl::context::tls_client};
        tls.set_default_verify_paths();
        tls.set_verify_mode(ssl::verify_peer);

        Tcp::resolver resolver{context};
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream{
            context,
            tls};
        const std::string host{kWebSocketHost};
        if (!SSL_set_tlsext_host_name(
                stream.next_layer().native_handle(),
                host.c_str())) {
            throw std::runtime_error(
                "Failed to configure Binance Alpha TLS server name");
        }
        stream.next_layer().set_verify_callback(
            ssl::host_name_verification(host));

        const auto endpoints = resolver.resolve(host, kWebSocketPort);
        beast::get_lowest_layer(stream).expires_after(
            config_.http_timeout);
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.next_layer().handshake(ssl::stream_base::client);

        websocket::stream_base::timeout timeouts =
            websocket::stream_base::timeout::suggested(
                beast::role_type::client);
        timeouts.handshake_timeout = config_.http_timeout;
        timeouts.idle_timeout = std::chrono::seconds{5};
        timeouts.keep_alive_pings = true;
        stream.set_option(timeouts);
        stream.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& request) {
                request.set(
                    boost::beast::http::field::user_agent,
                    "exchange-agent-native/1");
            }));
        stream.handshake(host, binance_alpha_stream_path(metadata));
        const std::string subscription =
            binance_alpha_subscription_message(metadata);
        stream.write(asio::buffer(subscription));

        while (!stop_requested_) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            bool completed = false;
            stream.async_read(
                buffer,
                [&](boost::system::error_code read_error, std::size_t) {
                    error = read_error;
                    completed = true;
                });
            while (!completed && !stop_requested_) {
                static_cast<void>(context.run_for(
                    std::chrono::milliseconds{100}));
                context.restart();
            }
            if (!completed) {
                boost::system::error_code ignored;
                beast::get_lowest_layer(stream).socket().cancel(ignored);
                context.run();
                return;
            }
            if (error == beast::error::timeout) {
                return;
            }
            if (error) {
                throw boost::system::system_error(error);
            }
            const std::string message =
                beast::buffers_to_string(buffer.data());
            if (is_subscription_acknowledgement(message)) {
                continue;
            }
            try {
                ingest_message(message, system_time_millis());
            } catch (const BinanceAlphaParseError& error) {
                set_error(error.what());
            }
        }

        boost::system::error_code ignored;
        stream.close(websocket::close_code::normal, ignored);
    }

    void BinanceAlphaMarketFeed::set_error(std::string message) {
        std::lock_guard lock{mutex_};
        last_error_ = std::move(message);
    }
}  // namespace exchange
