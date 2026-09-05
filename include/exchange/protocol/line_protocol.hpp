#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "exchange/matching/command.hpp"
#include "exchange/matching/event.hpp"

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
        InvalidOrder,
        CancelNotFound,
        LineTooLong,
    };

    struct ProtocolError {
        ProtocolErrorCode code{ProtocolErrorCode::MalformedCommand};
        OrderId order_id{};

        bool operator==(const ProtocolError&) const = default;
    };

    using CommandParseResult = std::variant<Command, ProtocolError>;

    [[nodiscard]] CommandParseResult parse_command(std::string_view line);
    [[nodiscard]] std::string encode_success(std::span<const Event> events);
    [[nodiscard]] std::string encode_error(ProtocolError error);
}  // namespace exchange
