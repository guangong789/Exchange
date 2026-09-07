#include "exchange/protocol/line_protocol.hpp"

#include <charconv>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace exchange {
    namespace {
        std::vector<std::string_view> split_tokens(std::string_view line) {
            std::vector<std::string_view> tokens;
            if (line.empty() || line.front() == ' ' || line.back() == ' ') {
                return tokens;
            }

            std::size_t token_start = 0;
            while (token_start < line.size()) {
                const std::size_t separator = line.find(' ', token_start);
                const std::size_t token_end =
                    separator == std::string_view::npos ? line.size() : separator;
                if (token_end == token_start) {
                    tokens.clear();
                    return tokens;
                }

                tokens.push_back(line.substr(token_start, token_end - token_start));
                if (separator == std::string_view::npos) {
                    break;
                }
                token_start = separator + 1;
            }
            return tokens;
        }

        template <typename Integer>
        bool parse_integer(std::string_view token, Integer& value) {
            if (token.empty()) {
                return false;
            }

            const char* const first = token.data();
            const char* const last = token.data() + token.size();
            const auto [parsed_to, error] = std::from_chars(first, last, value);
            return error == std::errc{} && parsed_to == last;
        }

        std::string_view side_name(Side side) {
            return side == Side::Buy ? "BUY" : "SELL";
        }

        void append_order(std::string& output, const Order& order) {
            output += std::to_string(order.id);
            output += ' ';
            output += side_name(order.side);
            output += ' ';
            output += std::to_string(order.price);
            output += ' ';
            output += std::to_string(order.quantity);
            output += ' ';
            output += std::to_string(order.timestamp);
            output += '\n';
        }

        void append_event(std::string& output, const Event& event) {
            std::visit(
                [&output](const auto& payload) {
                    using Payload = std::decay_t<decltype(payload)>;

                    if constexpr (std::is_same_v<Payload, OrderAccepted>) {
                        output += "EVENT ORDER_ACCEPTED ";
                        append_order(output, payload.order);
                    } else if constexpr (std::is_same_v<Payload, OrderCancelled>) {
                        output += "EVENT ORDER_CANCELLED ";
                        append_order(output, payload.order);
                    } else if constexpr (std::is_same_v<Payload, TradeCreated>) {
                        output += "EVENT TRADE_CREATED ";
                        output += std::to_string(payload.trade.buy_order_id);
                        output += ' ';
                        output += std::to_string(payload.trade.sell_order_id);
                        output += ' ';
                        output += std::to_string(payload.trade.price);
                        output += ' ';
                        output += std::to_string(payload.trade.quantity);
                        output += ' ';
                        output += std::to_string(payload.trade.timestamp);
                        output += '\n';
                    } else if constexpr (std::is_same_v<Payload, OrderFilled>) {
                        output += "EVENT ORDER_FILLED ";
                        output += std::to_string(payload.order_id);
                        output += ' ';
                        output += side_name(payload.side);
                        output += ' ';
                        output += std::to_string(payload.filled_quantity);
                        output += '\n';
                    } else if constexpr (
                        std::is_same_v<Payload, OrderPartiallyFilled>) {
                        output += "EVENT ORDER_PARTIALLY_FILLED ";
                        output += std::to_string(payload.order_id);
                        output += ' ';
                        output += side_name(payload.side);
                        output += ' ';
                        output += std::to_string(payload.filled_quantity);
                        output += ' ';
                        output += std::to_string(payload.remaining_quantity);
                        output += '\n';
                    }
                },
                event.payload);
        }

        ProtocolError malformed_command() {
            return ProtocolError{ProtocolErrorCode::MalformedCommand};
        }

        std::string_view trading_result_name(TradingResult result) {
            switch (result) {
                case TradingResult::Accepted:
                    return "ACCEPTED";
                case TradingResult::Cancelled:
                    return "CANCELLED";
                case TradingResult::AccountNotFound:
                    return "ACCOUNT_NOT_FOUND";
                case TradingResult::InsufficientFunds:
                    return "INSUFFICIENT_FUNDS";
                case TradingResult::DuplicateOrder:
                    return "DUPLICATE_ORDER";
                case TradingResult::InvalidOrder:
                    return "INVALID_ORDER";
                case TradingResult::CounterpartyNotAccountBacked:
                    return "COUNTERPARTY_NOT_ACCOUNT_BACKED";
                case TradingResult::CancelNotFound:
                    return "CANCEL_NOT_FOUND";
                case TradingResult::CancelNotOwner:
                    return "CANCEL_NOT_OWNER";
                case TradingResult::InvalidRequest:
                    return "INVALID_REQUEST";
            }
            throw std::invalid_argument("unknown trading result");
        }

        bool is_successful(TradingResult result) {
            return result == TradingResult::Accepted
                || result == TradingResult::Cancelled;
        }
    }  // namespace

    LineFramer::LineFramer(std::size_t max_line_length)
        : max_line_length_(max_line_length) {
        if (max_line_length_ == 0) {
            throw std::invalid_argument("maximum line length must be positive");
        }
    }

    void LineFramer::append(std::string_view bytes) {
        buffer_.append(bytes);
    }

    LineFrameResult LineFramer::next_line() {
        if (line_too_long_) {
            return LineFrameResult{LineFrameStatus::LineTooLong, {}};
        }

        const std::size_t newline = buffer_.find('\n', read_position_);
        if (newline == std::string::npos) {
            const std::size_t pending_length = buffer_.size() - read_position_;
            const bool possible_trailing_carriage_return =
                pending_length == max_line_length_ + 1 &&
                buffer_.back() == '\r';
            if (pending_length > max_line_length_ &&
                !possible_trailing_carriage_return) {
                line_too_long_ = true;
                return LineFrameResult{LineFrameStatus::LineTooLong, {}};
            }
            return LineFrameResult{LineFrameStatus::NeedMoreData, {}};
        }

        std::size_t line_length = newline - read_position_;
        if (line_length > 0 && buffer_[newline - 1] == '\r') {
            --line_length;
        }
        if (line_length > max_line_length_) {
            line_too_long_ = true;
            return LineFrameResult{LineFrameStatus::LineTooLong, {}};
        }

        LineFrameResult result{
            LineFrameStatus::LineReady,
            buffer_.substr(read_position_, line_length),
        };
        read_position_ = newline + 1;
        compact();
        return result;
    }

    void LineFramer::compact() {
        if (read_position_ == buffer_.size()) {
            buffer_.clear();
            read_position_ = 0;
            return;
        }

        if (read_position_ >= 4096 && read_position_ >= buffer_.size() / 2) {
            buffer_.erase(0, read_position_);
            read_position_ = 0;
        }
    }

    TradingRequestParseResult parse_trading_request(
        std::string_view line) {
        const std::vector<std::string_view> tokens = split_tokens(line);
        if (tokens.empty()) {
            return malformed_command();
        }

        if (tokens[0] == "ADD") {
            if (tokens.size() != 6) {
                return malformed_command();
            }

            RequestId request_id{};
            AccountId account_id{};
            if (!parse_integer(tokens[1], request_id)
                || !parse_integer(tokens[2], account_id)) {
                return malformed_command();
            }
            Side side;
            if (tokens[3] == "BUY") {
                side = Side::Buy;
            } else if (tokens[3] == "SELL") {
                side = Side::Sell;
            } else {
                return malformed_command();
            }
            Price price{};
            Quantity quantity{};
            if (!parse_integer(tokens[4], price)
                || !parse_integer(tokens[5], quantity)) {
                return malformed_command();
            }
            if (request_id == 0 || account_id == 0
                || price <= 0 || quantity <= 0) {
                return malformed_command();
            }

            return TradingRequest{
                request_id,
                account_id,
                SubmitTradingRequest{side, price, quantity}};
        }

        if (tokens[0] == "CANCEL") {
            if (tokens.size() != 4) {
                return malformed_command();
            }

            RequestId request_id{};
            AccountId account_id{};
            OrderId order_id{};
            if (!parse_integer(tokens[1], request_id)
                || !parse_integer(tokens[2], account_id)
                || !parse_integer(tokens[3], order_id)
                || request_id == 0 || account_id == 0 || order_id == 0) {
                return malformed_command();
            }
            return TradingRequest{
                request_id,
                account_id,
                CancelTradingRequest{order_id}};
        }

        return malformed_command();
    }

    std::string encode_trading_response(
        const TradingResponse& response) {
        const bool include_events = is_successful(response.result);
        const std::size_t event_count =
            include_events ? response.events.size() : 0;
        OrderId assigned_order_id = 0;
        if (response.result == TradingResult::Accepted) {
            if (!response.assigned_order_id.has_value()
                || *response.assigned_order_id == 0) {
                throw std::invalid_argument(
                    "accepted response is missing assigned order ID");
            }
            assigned_order_id = *response.assigned_order_id;
        }

        std::string output = "RESULT ";
        output += std::to_string(response.request_id);
        output += ' ';
        output += trading_result_name(response.result);
        output += ' ';
        output += std::to_string(assigned_order_id);
        output += ' ';
        output += std::to_string(event_count);
        output += '\n';

        if (include_events) {
            for (const Event& event : response.events) {
                append_event(output, event);
            }
        }
        return output;
    }

    std::string encode_error(ProtocolError error) {
        switch (error.code) {
            case ProtocolErrorCode::MalformedCommand:
                return "ERR MALFORMED_COMMAND\n";
            case ProtocolErrorCode::LineTooLong:
                return "ERR LINE_TOO_LONG\n";
        }
        throw std::invalid_argument("unknown protocol error code");
    }
}  // namespace exchange
