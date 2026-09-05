#include "exchange/protocol/line_protocol.hpp"

#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        const Command& parsed_command(const CommandParseResult& result) {
            return std::get<Command>(result);
        }

        void expect_malformed(const CommandParseResult& result) {
            ASSERT_TRUE(std::holds_alternative<ProtocolError>(result));
            EXPECT_EQ(std::get<ProtocolError>(result).code,
                      ProtocolErrorCode::MalformedCommand);
        }

        TEST(LineFramerTest, WaitsForACompleteLineAcrossAppends) {
            LineFramer framer;
            framer.append("ADD 1 BUY 100");

            EXPECT_EQ(framer.next_line().status,
                      LineFrameStatus::NeedMoreData);

            framer.append(" 5 10\n");
            const LineFrameResult result = framer.next_line();
            EXPECT_EQ(result.status, LineFrameStatus::LineReady);
            EXPECT_EQ(result.line, "ADD 1 BUY 100 5 10");
            EXPECT_EQ(framer.next_line().status,
                      LineFrameStatus::NeedMoreData);
        }

        TEST(LineFramerTest, ReturnsMultipleLinesAndKeepsIncompleteTail) {
            LineFramer framer;
            framer.append("CANCEL 1\nCANCEL 2\nADD 3 SELL");

            const LineFrameResult first = framer.next_line();
            const LineFrameResult second = framer.next_line();
            EXPECT_EQ(first.status, LineFrameStatus::LineReady);
            EXPECT_EQ(first.line, "CANCEL 1");
            EXPECT_EQ(second.status, LineFrameStatus::LineReady);
            EXPECT_EQ(second.line, "CANCEL 2");
            EXPECT_EQ(framer.next_line().status,
                      LineFrameStatus::NeedMoreData);

            framer.append(" 101 4 20\n");
            const LineFrameResult third = framer.next_line();
            EXPECT_EQ(third.status, LineFrameStatus::LineReady);
            EXPECT_EQ(third.line, "ADD 3 SELL 101 4 20");
        }

        TEST(LineFramerTest, AcceptsCrLfAndReturnsEmptyLines) {
            LineFramer framer;
            framer.append("CANCEL 7\r\n\n");

            EXPECT_EQ(framer.next_line().line, "CANCEL 7");
            const LineFrameResult empty = framer.next_line();
            EXPECT_EQ(empty.status, LineFrameStatus::LineReady);
            EXPECT_TRUE(empty.line.empty());
        }

        TEST(LineFramerTest, EnforcesMaximumLineLength) {
            LineFramer exact_limit(4);
            exact_limit.append("ABCD\r\n");
            EXPECT_EQ(exact_limit.next_line().status,
                      LineFrameStatus::LineReady);

            LineFramer too_long(4);
            too_long.append("ABCDE");
            EXPECT_EQ(too_long.next_line().status,
                      LineFrameStatus::LineTooLong);
            EXPECT_EQ(too_long.next_line().status,
                      LineFrameStatus::LineTooLong);
        }

        TEST(LineFramerTest, RejectsZeroMaximumLineLength) {
            EXPECT_THROW(LineFramer(0), std::invalid_argument);
        }

        TEST(LineFramerTest, KeepsAnIncompleteTailAfterConsumedPrefixCompaction) {
            LineFramer framer;
            std::string input;
            for (int i = 0; i < 700; ++i) {
                input += "CANCEL 1\n";
            }
            input += "ADD 2 BUY";
            framer.append(input);

            for (int i = 0; i < 700; ++i) {
                const LineFrameResult result = framer.next_line();
                ASSERT_EQ(result.status, LineFrameStatus::LineReady);
                EXPECT_EQ(result.line, "CANCEL 1");
            }
            EXPECT_EQ(framer.next_line().status,
                      LineFrameStatus::NeedMoreData);

            framer.append(" 100 3 20\n");
            const LineFrameResult result = framer.next_line();
            EXPECT_EQ(result.status, LineFrameStatus::LineReady);
            EXPECT_EQ(result.line, "ADD 2 BUY 100 3 20");
        }

        TEST(LineProtocolParserTest, ParsesAddOrder) {
            const CommandParseResult result =
                parse_command("ADD 42 SELL 100500 7 123456");

            ASSERT_TRUE(std::holds_alternative<Command>(result));
            const auto& add =
                std::get<AddOrder>(parsed_command(result).payload);
            EXPECT_EQ(add.order.id, 42U);
            EXPECT_EQ(add.order.side, Side::Sell);
            EXPECT_EQ(add.order.type, OrderType::Limit);
            EXPECT_EQ(add.order.price, 100500);
            EXPECT_EQ(add.order.quantity, 7);
            EXPECT_EQ(add.order.timestamp, 123456);
        }

        TEST(LineProtocolParserTest, ParsesCancelOrder) {
            const CommandParseResult result = parse_command("CANCEL 99");

            ASSERT_TRUE(std::holds_alternative<Command>(result));
            const auto& cancel =
                std::get<CancelOrder>(parsed_command(result).payload);
            EXPECT_EQ(cancel.order_id, 99U);
        }

        TEST(LineProtocolParserTest, LeavesBusinessValidationToMatchingEngine) {
            const CommandParseResult result =
                parse_command("ADD 0 BUY 0 -3 -1");

            ASSERT_TRUE(std::holds_alternative<Command>(result));
            const auto& add =
                std::get<AddOrder>(parsed_command(result).payload);
            EXPECT_EQ(add.order.id, 0U);
            EXPECT_EQ(add.order.price, 0);
            EXPECT_EQ(add.order.quantity, -3);
            EXPECT_EQ(add.order.timestamp, -1);
        }

        TEST(LineProtocolParserTest, RejectsMalformedGrammar) {
            const std::vector<std::string> malformed{
                "",
                "ADD 1 BUY 100 5",
                "ADD 1 BUY 100 5 10 extra",
                " ADD 1 BUY 100 5 10",
                "ADD 1 BUY 100 5 10 ",
                "ADD  1 BUY 100 5 10",
                "ADD\t1 BUY 100 5 10",
                "ADD 1 buy 100 5 10",
                "ADD 1 HOLD 100 5 10",
                "CANCEL",
                "CANCEL 1 2",
                "cancel 1",
                "CANCEL +1",
                "CANCEL 1\r",
                "UNKNOWN 1",
            };

            for (const std::string& line : malformed) {
                expect_malformed(parse_command(line));
            }
        }

        TEST(LineProtocolParserTest, RejectsIntegerOverflow) {
            const std::string too_large_order_id =
                std::to_string(std::numeric_limits<OrderId>::max()) + "0";
            expect_malformed(parse_command("CANCEL " + too_large_order_id));
            expect_malformed(
                parse_command("ADD 1 BUY 9223372036854775808 1 1"));
        }

        TEST(LineProtocolEncoderTest, EncodesAllEventTypesInOrder) {
            const std::vector<Event> events{
                Event{EventPayload{OrderAccepted{
                    Order{1, Side::Buy, OrderType::Limit, 100, 7, 10}}}},
                Event{EventPayload{TradeCreated{
                    Trade{1, 2, 99, 3, 11}}}},
                Event{EventPayload{OrderFilled{
                    2, Side::Sell, 3}}},
                Event{EventPayload{OrderPartiallyFilled{
                    1, Side::Buy, 3, 4}}},
                Event{EventPayload{OrderCancelled{
                    Order{1, Side::Buy, OrderType::Limit, 100, 4, 10}}}},
            };

            EXPECT_EQ(
                encode_success(events),
                "OK 5\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 7 10\n"
                "EVENT TRADE_CREATED 1 2 99 3 11\n"
                "EVENT ORDER_FILLED 2 SELL 3\n"
                "EVENT ORDER_PARTIALLY_FILLED 1 BUY 3 4\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 4 10\n");
        }

        TEST(LineProtocolEncoderTest, EncodesEmptySuccessAndErrors) {
            EXPECT_EQ(encode_success(std::span<const Event>{}), "OK 0\n");
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::MalformedCommand, 0}),
                "ERR MALFORMED_COMMAND\n");
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::InvalidOrder, 0}),
                "ERR INVALID_ORDER\n");
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::CancelNotFound, 42}),
                "ERR CANCEL_NOT_FOUND 42\n");
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::LineTooLong, 0}),
                "ERR LINE_TOO_LONG\n");
        }
    }  // namespace
}  // namespace exchange
