#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

#include "exchange/execution/trading_request.hpp"

namespace exchange {
    inline constexpr std::size_t kMaxProtocolLineLength = 256;

    enum class LineFrameStatus {
        NeedMoreData,
        LineReady,
        LineTooLong,
    };

    struct LineFrameResult {
        LineFrameStatus status{LineFrameStatus::NeedMoreData};
        std::string line;
    };

    class LineFramer {
    public:
        explicit LineFramer(
            std::size_t max_line_length = kMaxProtocolLineLength);

        void append(std::string_view bytes);
        [[nodiscard]] LineFrameResult next_line();

    private:
        void compact();

        std::size_t max_line_length_;
        std::string buffer_;
        std::size_t read_position_{};
        bool line_too_long_{};
    };

    enum class ProtocolErrorCode {
        MalformedCommand,
        LineTooLong,
    };

    struct ProtocolError {
        ProtocolErrorCode code{ProtocolErrorCode::MalformedCommand};

        bool operator==(const ProtocolError&) const = default;
    };

    using TradingRequestParseResult =
        std::variant<TradingRequest, ProtocolError>;

    [[nodiscard]] TradingRequestParseResult parse_trading_request(
        std::string_view line);
    [[nodiscard]] std::string encode_trading_response(
        const TradingResponse& response);

    [[nodiscard]] std::string encode_error(ProtocolError error);
}  // namespace exchange
