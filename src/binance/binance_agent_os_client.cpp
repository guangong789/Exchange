#include "exchange/binance/binance_agent_os_client.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace exchange {
    namespace {
#if defined(__SIZEOF_INT128__)
        __extension__ using WideUnsigned = unsigned __int128;
#else
#error "exact external price conversion requires unsigned 128-bit integers"
#endif

        using Json = nlohmann::json;
        constexpr std::size_t max_response_size = 2 * 1024 * 1024;
        class CurlGlobalState {
        public:
            CurlGlobalState() {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                    throw BinanceAgentOsTransportError(
                        "failed to initialize Binance MCP HTTP transport");
                }
            }

            ~CurlGlobalState() {
                curl_global_cleanup();
            }
        };

        struct HttpResponse {
            long status{};
            std::string body;
            std::string session_id;
            bool exceeded_limit{};
        };

        std::size_t append_body(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > std::numeric_limits<std::size_t>::max()
                    / size) {
                return 0;
            }
            const std::size_t byte_count = size * count;
            auto& response = *static_cast<HttpResponse*>(context);
            if (byte_count > max_response_size - response.body.size()) {
                response.exceeded_limit = true;
                return 0;
            }
            try {
                response.body.append(data, byte_count);
                return byte_count;
            } catch (...) {
                return 0;
            }
        }

        std::string_view trim_header_value(std::string_view value) {
            while (!value.empty()
                   && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty()
                   && (value.back() == '\r' || value.back() == '\n'
                       || value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            return value;
        }

        bool starts_with_case_insensitive(
            std::string_view value,
            std::string_view prefix) {
            if (value.size() < prefix.size()) {
                return false;
            }
            for (std::size_t index = 0; index < prefix.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(value[index]))
                    != std::tolower(
                        static_cast<unsigned char>(prefix[index]))) {
                    return false;
                }
            }
            return true;
        }

        std::size_t capture_header(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > std::numeric_limits<std::size_t>::max()
                    / size) {
                return 0;
            }
            const std::size_t byte_count = size * count;
            auto& response = *static_cast<HttpResponse*>(context);
            try {
                const std::string_view line(data, byte_count);
                constexpr std::string_view header_name = "mcp-session-id:";
                if (starts_with_case_insensitive(line, header_name)) {
                    response.session_id = std::string(trim_header_value(
                        line.substr(header_name.size())));
                }
                return byte_count;
            } catch (...) {
                return 0;
            }
        }

        CurlGlobalState& curl_global_state() {
            static CurlGlobalState state;
            return state;
        }

        std::string configured_access_token() {
            const char* token = std::getenv(
                "BINANCE_AGENT_OS_ACCESS_TOKEN");
            return token == nullptr ? std::string{} : std::string(token);
        }

        HttpResponse post_json(
            const BinanceMcpConfig& config,
            const Json& payload,
            const std::string& access_token,
            std::string_view session_id) {
            using CurlHandle =
                std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
            using CurlHeaders =
                std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

            CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
            if (!handle) {
                throw BinanceAgentOsTransportError(
                    "failed to create Binance MCP HTTP request");
            }

            const std::string body = payload.dump();
            const std::string authorization =
                "Authorization: Bearer " + access_token;
            const std::string session_header =
                "Mcp-Session-Id: " + std::string(session_id);

            CurlHeaders headers(nullptr, &curl_slist_free_all);
            const auto append_header = [&headers](const char* value) {
                curl_slist* updated = curl_slist_append(headers.get(), value);
                if (updated == nullptr) {
                    throw BinanceAgentOsTransportError(
                        "failed to allocate Binance MCP HTTP headers");
                }
                static_cast<void>(headers.release());
                headers.reset(updated);
            };
            append_header("Content-Type: application/json");
            append_header("Accept: application/json, text/event-stream");
            if (!access_token.empty()) {
                append_header(authorization.c_str());
            }
            append_header("MCP-Protocol-Version: 2025-06-18");
            if (!session_id.empty()) {
                append_header(session_header.c_str());
            }

            HttpResponse response;
            char error_buffer[CURL_ERROR_SIZE]{};
            const auto set_option = [&handle](CURLoption option, auto value) {
                if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) {
                    throw BinanceAgentOsTransportError(
                        "failed to configure Binance MCP HTTP request");
                }
            };
            set_option(CURLOPT_URL, config.endpoint.c_str());
            set_option(CURLOPT_HTTPHEADER, headers.get());
            set_option(CURLOPT_POST, 1L);
            set_option(CURLOPT_POSTFIELDS, body.c_str());
            set_option(
                CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(body.size()));
            set_option(CURLOPT_WRITEFUNCTION, &append_body);
            set_option(CURLOPT_WRITEDATA, &response);
            set_option(CURLOPT_HEADERFUNCTION, &capture_header);
            set_option(CURLOPT_HEADERDATA, &response);
            set_option(
                CURLOPT_TIMEOUT_MS,
                static_cast<long>(config.timeout.count()));
            set_option(
                CURLOPT_CONNECTTIMEOUT_MS,
                static_cast<long>(config.timeout.count()));
            set_option(CURLOPT_NOSIGNAL, 1L);
            set_option(CURLOPT_ERRORBUFFER, error_buffer);

            const CURLcode result = curl_easy_perform(handle.get());
            if (result != CURLE_OK) {
                if (response.exceeded_limit) {
                    throw BinanceAgentOsTransportError(
                        "Binance MCP response exceeded size limit");
                }
                const char* detail = error_buffer[0] == '\0'
                    ? curl_easy_strerror(result)
                    : error_buffer;
                throw BinanceAgentOsTransportError(
                    std::string("Binance MCP transport failure: ") + detail);
            }
            if (curl_easy_getinfo(
                    handle.get(),
                    CURLINFO_RESPONSE_CODE,
                    &response.status)
                != CURLE_OK) {
                throw BinanceAgentOsTransportError(
                    "failed to read Binance MCP HTTP status");
            }
            if (response.status == 401 || response.status == 403) {
                throw BinanceAgentOsAuthenticationError(
                    "Binance Agent OS MCP requires OAuth access token to establish a session");
            }
            if (response.status < 200 || response.status >= 300) {
                throw BinanceAgentOsTransportError(
                    "Binance MCP returned HTTP status "
                    + std::to_string(response.status));
            }
            return response;
        }

        Json parse_mcp_body(const std::string& body) {
            try {
                if (!body.empty() && body.front() == '{') {
                    return Json::parse(body);
                }

                std::size_t position = 0;
                while (position < body.size()) {
                    const std::size_t end = body.find('\n', position);
                    std::string_view line(
                        body.data() + position,
                        (end == std::string::npos ? body.size() : end)
                            - position);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1);
                    }
                    constexpr std::string_view data_prefix = "data:";
                    if (line.starts_with(data_prefix)) {
                        const std::string_view data = trim_header_value(
                            line.substr(data_prefix.size()));
                        if (!data.empty()) {
                            return Json::parse(data);
                        }
                    }
                    if (end == std::string::npos) {
                        break;
                    }
                    position = end + 1;
                }
            } catch (const Json::exception&) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP returned malformed JSON");
            }
            throw BinanceAgentOsProviderError(
                "Binance MCP returned no JSON-RPC message");
        }

        const Json& require_result(const Json& message) {
            if (!message.is_object()) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP response is not an object");
            }
            const auto error = message.find("error");
            if (error != message.end()) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP returned a JSON-RPC error");
            }
            const auto result = message.find("result");
            if (result == message.end() || !result->is_object()) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP response has no result object");
            }
            return *result;
        }

        std::string lower_copy(std::string value) {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        }

        bool contains_any(
            const std::string& value,
            std::initializer_list<std::string_view> needles) {
            return std::any_of(
                needles.begin(),
                needles.end(),
                [&value](std::string_view needle) {
                    return value.find(needle) != std::string::npos;
                });
        }

        std::string discover_market_tool(const Json& tools_result) {
            const auto tools = tools_result.find("tools");
            if (tools == tools_result.end() || !tools->is_array()) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP tools/list has no tools array");
            }

            int best_score = 0;
            std::string best_name;
            for (const Json& tool : *tools) {
                if (!tool.is_object()) {
                    continue;
                }
                const auto name = tool.find("name");
                const auto schema = tool.find("inputSchema");
                if (name == tool.end() || !name->is_string()
                    || schema == tool.end() || !schema->is_object()) {
                    continue;
                }
                const auto properties = schema->find("properties");
                if (properties == schema->end() || !properties->is_object()
                    || properties->find("symbol") == properties->end()) {
                    continue;
                }

                const auto annotations = tool.find("annotations");
                if (annotations != tool.end() && annotations->is_object()) {
                    const auto read_only = annotations->find("readOnlyHint");
                    if (read_only != annotations->end()
                        && read_only->is_boolean()
                        && !read_only->get<bool>()) {
                        continue;
                    }
                }

                std::string searchable = name->get<std::string>();
                const auto description = tool.find("description");
                if (description != tool.end() && description->is_string()) {
                    searchable += " " + description->get<std::string>();
                }
                searchable = lower_copy(std::move(searchable));
                if (contains_any(
                        searchable,
                        {"place order", "create order", "cancel order",
                         "transfer", "withdraw"})) {
                    continue;
                }

                int score = 0;
                if (contains_any(
                        searchable,
                        {"best bid", "best ask", "book ticker"})) {
                    score = 100;
                } else if (searchable.find("order book")
                           != std::string::npos) {
                    score = 80;
                } else if (searchable.find("ticker")
                           != std::string::npos) {
                    score = 40;
                }
                if (score > best_score) {
                    best_score = score;
                    best_name = name->get<std::string>();
                }
            }
            if (best_name.empty()) {
                throw BinanceAgentOsProviderError(
                    "Binance MCP exposes no discoverable read-only best-quote tool");
            }
            return best_name;
        }

        std::optional<std::string> string_price(
            const Json& object,
            std::initializer_list<std::string_view> keys) {
            for (std::string_view key : keys) {
                const auto value = object.find(std::string(key));
                if (value != object.end() && value->is_string()) {
                    return value->get<std::string>();
                }
                if (value != object.end() && value->is_number_integer()) {
                    return value->dump();
                }
            }
            return std::nullopt;
        }

        struct RawQuote {
            std::string symbol;
            std::string bid;
            std::string ask;
        };

        std::optional<RawQuote> find_raw_quote(
            const Json& value,
            std::string_view requested_symbol) {
            if (value.is_object()) {
                const auto bid = string_price(
                    value,
                    {"bidPrice", "bestBidPrice", "best_bid", "bid"});
                const auto ask = string_price(
                    value,
                    {"askPrice", "bestAskPrice", "best_ask", "ask"});
                if (bid.has_value() && ask.has_value()) {
                    std::string symbol(requested_symbol);
                    const auto supplied_symbol = value.find("symbol");
                    if (supplied_symbol != value.end()
                        && supplied_symbol->is_string()) {
                        symbol = supplied_symbol->get<std::string>();
                    }
                    return RawQuote{symbol, *bid, *ask};
                }

                const auto bids = value.find("bids");
                const auto asks = value.find("asks");
                if (bids != value.end() && asks != value.end()
                    && bids->is_array() && asks->is_array()
                    && !bids->empty() && !asks->empty()
                    && (*bids)[0].is_array() && (*asks)[0].is_array()
                    && !(*bids)[0].empty() && !(*asks)[0].empty()
                    && (*bids)[0][0].is_string()
                    && (*asks)[0][0].is_string()) {
                    return RawQuote{
                        std::string(requested_symbol),
                        (*bids)[0][0].get<std::string>(),
                        (*asks)[0][0].get<std::string>()};
                }

                for (const auto& item : value.items()) {
                    if (const auto quote = find_raw_quote(
                            item.value(), requested_symbol)) {
                        return quote;
                    }
                }
            } else if (value.is_array()) {
                for (const Json& item : value) {
                    if (const auto quote = find_raw_quote(
                            item, requested_symbol)) {
                        return quote;
                    }
                }
            }
            return std::nullopt;
        }

        Json unpack_tool_payload(const Json& result) {
            const auto is_error = result.find("isError");
            if (is_error != result.end() && is_error->is_boolean()
                && is_error->get<bool>()) {
                throw BinanceAgentOsProviderError(
                    "Binance market-data tool returned an error");
            }
            const auto structured = result.find("structuredContent");
            if (structured != result.end()) {
                return *structured;
            }
            const auto content = result.find("content");
            if (content == result.end() || !content->is_array()) {
                throw BinanceAgentOsProviderError(
                    "Binance market-data result has no content");
            }
            for (const Json& item : *content) {
                if (!item.is_object()) {
                    continue;
                }
                const auto text = item.find("text");
                if (text != item.end() && text->is_string()) {
                    try {
                        return Json::parse(text->get_ref<const std::string&>());
                    } catch (const Json::exception&) {
                        continue;
                    }
                }
            }
            throw BinanceAgentOsProviderError(
                "Binance market-data content has no structured quote");
        }

        WideUnsigned parse_digits(std::string_view digits) {
            constexpr WideUnsigned maximum = ~WideUnsigned{0};
            WideUnsigned value = 0;
            for (char character : digits) {
                const WideUnsigned digit = static_cast<unsigned>(
                    character - '0');
                if (value > (maximum - digit) / 10) {
                    throw std::overflow_error("external decimal is too large");
                }
                value = value * 10 + digit;
            }
            return value;
        }

        WideUnsigned greatest_common_divisor(
            WideUnsigned left,
            WideUnsigned right) {
            while (right != 0) {
                const WideUnsigned remainder = left % right;
                left = right;
                right = remainder;
            }
            return left;
        }
    }  // namespace

    Price parse_scaled_decimal_price(
        std::string_view decimal,
        Price local_price_scale) {
        if (local_price_scale <= 0) {
            throw std::invalid_argument("local price scale must be positive");
        }
        if (decimal.empty()) {
            throw std::invalid_argument("external price is empty");
        }

        const std::size_t decimal_point = decimal.find('.');
        if (decimal_point == 0 || decimal_point == decimal.size() - 1
            || (decimal_point != std::string_view::npos
                && decimal.find('.', decimal_point + 1)
                    != std::string_view::npos)) {
            throw std::invalid_argument("external price is not a decimal");
        }
        for (char character : decimal) {
            if (character != '.'
                && (character < '0' || character > '9')) {
                throw std::invalid_argument(
                    "external price is not a decimal");
            }
        }

        const std::string_view integer_digits = decimal.substr(
            0,
            decimal_point == std::string_view::npos
                ? decimal.size()
                : decimal_point);
        const WideUnsigned integer_part = parse_digits(integer_digits);
        const WideUnsigned scale = static_cast<WideUnsigned>(
            local_price_scale);
        const WideUnsigned maximum = static_cast<WideUnsigned>(
            std::numeric_limits<Price>::max());
        if (integer_part > maximum / scale) {
            throw std::overflow_error("scaled external price overflow");
        }
        WideUnsigned scaled = integer_part * scale;

        if (decimal_point != std::string_view::npos) {
            std::string_view fractional_digits = decimal.substr(
                decimal_point + 1);
            while (!fractional_digits.empty()
                   && fractional_digits.back() == '0') {
                fractional_digits.remove_suffix(1);
            }
            if (!fractional_digits.empty()) {
                if (fractional_digits.size() > 38) {
                    throw std::invalid_argument(
                        "external price has excessive precision");
                }
                WideUnsigned denominator = 1;
                for (std::size_t index = 0;
                     index < fractional_digits.size();
                     ++index) {
                    denominator *= 10;
                }
                const WideUnsigned fraction = parse_digits(
                    fractional_digits);
                const WideUnsigned divisor = greatest_common_divisor(
                    fraction, denominator);
                const WideUnsigned reduced_fraction = fraction / divisor;
                const WideUnsigned reduced_denominator = denominator / divisor;
                if (reduced_denominator > scale
                    || scale % reduced_denominator != 0) {
                    throw std::invalid_argument(
                        "external price has excessive precision");
                }
                const WideUnsigned reduced_scale =
                    scale / reduced_denominator;
                if (reduced_fraction != 0
                    && reduced_scale > (maximum - scaled)
                        / reduced_fraction) {
                    throw std::overflow_error(
                        "scaled external price overflow");
                }
                scaled += reduced_fraction * reduced_scale;
            }
        }
        if (scaled == 0) {
            throw std::invalid_argument(
                "external price must be positive");
        }
        return static_cast<Price>(scaled);
    }

    BinanceMcpClient::BinanceMcpClient(BinanceMcpConfig config)
        : config_(std::move(config)) {
        if (config_.endpoint.empty()) {
            throw std::invalid_argument(
                "Binance MCP endpoint must be non-empty");
        }
        if (config_.local_price_scale <= 0) {
            throw std::invalid_argument(
                "Binance local price scale must be positive");
        }
        if (config_.timeout.count() <= 0
            || config_.timeout.count() > LONG_MAX) {
            throw std::invalid_argument(
                "Binance MCP timeout must be positive and in range");
        }
        static_cast<void>(curl_global_state());
    }

    ExternalMarketSnapshot BinanceMcpClient::fetch_market_snapshot(
        std::string_view symbol) {
        if (symbol.empty()) {
            throw std::invalid_argument("Binance symbol must be non-empty");
        }
        const std::string access_token = configured_access_token();

        const Json initialize_request = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", "2025-06-18"},
              {"capabilities", Json::object()},
              {"clientInfo",
               {{"name", "exchange-binance-market-data-bridge"},
                {"version", "0"}}}}},
        };
        const HttpResponse initialized = post_json(
            config_, initialize_request, access_token, {});
        static_cast<void>(require_result(parse_mcp_body(initialized.body)));
        static_cast<void>(post_json(
            config_,
            Json{
                {"jsonrpc", "2.0"},
                {"method", "notifications/initialized"}},
            access_token,
            initialized.session_id));

        const HttpResponse listed = post_json(
            config_,
            Json{
                {"jsonrpc", "2.0"},
                {"id", 2},
                {"method", "tools/list"},
                {"params", Json::object()}},
            access_token,
            initialized.session_id);
        const std::string tool_name = discover_market_tool(
            require_result(parse_mcp_body(listed.body)));

        const HttpResponse called = post_json(
            config_,
            Json{
                {"jsonrpc", "2.0"},
                {"id", 3},
                {"method", "tools/call"},
                {"params",
                 {{"name", tool_name},
                  {"arguments", {{"symbol", std::string(symbol)}}}}}},
            access_token,
            initialized.session_id);
        const Json payload = unpack_tool_payload(
            require_result(parse_mcp_body(called.body)));
        const auto quote = find_raw_quote(payload, symbol);
        if (!quote.has_value()) {
            throw BinanceAgentOsProviderError(
                "Binance market-data response has no best bid/ask");
        }
        if (quote->symbol != symbol) {
            throw BinanceAgentOsProviderError(
                "Binance market-data response symbol does not match request");
        }

        ExternalMarketSnapshot snapshot{
            quote->symbol,
            parse_scaled_decimal_price(
                quote->bid, config_.local_price_scale),
            parse_scaled_decimal_price(
                quote->ask, config_.local_price_scale),
        };
        if (snapshot.best_bid > snapshot.best_ask) {
            throw BinanceAgentOsProviderError(
                "Binance market-data response has crossed best prices");
        }
        return snapshot;
    }

    BinanceAgentOsObservationBridge::BinanceAgentOsObservationBridge(
        BinanceAgentOsClient& client,
        std::string symbol)
        : client_(client), symbol_(std::move(symbol)) {
        if (symbol_.empty()) {
            throw std::invalid_argument("Binance symbol must be non-empty");
        }
    }

    AgentObservation BinanceAgentOsObservationBridge::attach_external_market(
        AgentObservation local_observation) {
        local_observation.external_market =
            client_.fetch_market_snapshot(symbol_);
        return local_observation;
    }
}  // namespace exchange
