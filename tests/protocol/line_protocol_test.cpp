#include "protocol/line_protocol.hpp"

#include <array>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        const TradingRequest& parsed_trading_request(
            const TradingRequestParseResult& result) {
            return std::get<TradingRequest>(result);
        }

        void expect_trading_malformed(
            const TradingRequestParseResult& result) {
            ASSERT_TRUE(std::holds_alternative<ProtocolError>(result));
            EXPECT_EQ(
                std::get<ProtocolError>(result).code,
                ProtocolErrorCode::MalformedCommand);
        }

        struct TradingResponseHeader {
            RequestId request_id{};
            std::string result;
            OrderId assigned_order_id{};
            std::size_t event_count{};
        };

        TradingResponseHeader parse_trading_response_header(
            std::string_view encoded) {
            const std::size_t newline = encoded.find('\n');
            if (newline == std::string_view::npos) {
                throw std::runtime_error("response header is incomplete");
            }

            std::istringstream input{std::string(encoded.substr(0, newline))};
            std::string marker;
            TradingResponseHeader header;
            if (!(input >> marker >> header.request_id >> header.result
                        >> header.assigned_order_id >> header.event_count)
                || marker != "RESULT") {
                throw std::runtime_error("response header is malformed");
            }

            std::string extra;
            if (input >> extra) {
                throw std::runtime_error("response header has extra fields");
            }
            return header;
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

        TEST(TradingLineProtocolParserTest,
             ParsesAccountAwareAddAndPreservesMetadata) {
            const TradingRequestParseResult result = parse_trading_request(
                "ADD 101 202 SELL 100500 7");

            ASSERT_TRUE(std::holds_alternative<TradingRequest>(result));
            const TradingRequest& request = parsed_trading_request(result);
            EXPECT_EQ(request.request_id, 101U);
            EXPECT_EQ(request.account_id, 202U);
            const auto& submit =
                std::get<SubmitTradingRequest>(request.payload);
            EXPECT_EQ(submit.side, Side::Sell);
            EXPECT_EQ(submit.price, 100500);
            EXPECT_EQ(submit.quantity, 7);
        }

        TEST(TradingLineProtocolParserTest,
             ParsesAccountAwareCancelAndPreservesMetadata) {
            const TradingRequestParseResult result =
                parse_trading_request("CANCEL 404 505 606");

            ASSERT_TRUE(std::holds_alternative<TradingRequest>(result));
            const TradingRequest& request = parsed_trading_request(result);
            EXPECT_EQ(request.request_id, 404U);
            EXPECT_EQ(request.account_id, 505U);
            EXPECT_EQ(
                std::get<CancelTradingRequest>(request.payload).order_id,
                606U);
        }

        TEST(TradingLineProtocolParserTest, RejectsMalformedTokenCounts) {
            const std::vector<std::string> malformed{
                "",
                "ADD 1 2 BUY 100",
                "ADD 1 2 BUY 100 4 extra",
                "CANCEL 1 2",
                "CANCEL 1 2 3 extra",
                "ADD 1 2 3 BUY 100 4 5",
                "ADD 1 BUY 100 4 5",
                "CANCEL 1",
            };

            for (const std::string& line : malformed) {
                expect_trading_malformed(parse_trading_request(line));
            }
        }

        TEST(TradingLineProtocolParserTest, RejectsInvalidNumericFields) {
            const std::vector<std::string> malformed{
                "ADD request 2 BUY 100 4",
                "ADD 1 account BUY 100 4",
                "ADD 1 2 BUY price 4",
                "ADD 1 2 BUY 100 quantity",
                "CANCEL request 2 3",
                "CANCEL 1 account 3",
                "CANCEL 1 2 order",
                "CANCEL 184467440737095516150 2 3",
            };

            for (const std::string& line : malformed) {
                expect_trading_malformed(parse_trading_request(line));
            }
        }

        TEST(TradingLineProtocolParserTest, RejectsZeroIdentifiers) {
            const std::vector<std::string> malformed{
                "ADD 0 2 BUY 100 4",
                "ADD 1 0 BUY 100 4",
                "CANCEL 0 2 3",
                "CANCEL 1 0 3",
                "CANCEL 1 2 0",
            };

            for (const std::string& line : malformed) {
                expect_trading_malformed(parse_trading_request(line));
            }
        }

        TEST(TradingLineProtocolParserTest,
             RejectsInvalidSidePriceAndQuantity) {
            const std::vector<std::string> malformed{
                "ADD 1 2 HOLD 100 4",
                "ADD 1 2 buy 100 4",
                "ADD 1 2 BUY 0 4",
                "ADD 1 2 BUY -1 4",
                "ADD 1 2 BUY 100 0",
                "ADD 1 2 BUY 100 -1",
            };

            for (const std::string& line : malformed) {
                expect_trading_malformed(parse_trading_request(line));
            }
        }

        TEST(TradingLineProtocolParserTest,
             DoesNotApplyAccountBalanceOwnershipOrDuplicateChecks) {
            EXPECT_TRUE(std::holds_alternative<TradingRequest>(
                parse_trading_request(
                    "ADD 1 999999 BUY 100 4")));
            EXPECT_TRUE(std::holds_alternative<TradingRequest>(
                parse_trading_request("CANCEL 2 999999 77")));
        }

        TEST(LineProtocolEncoderTest, EncodesProtocolErrors) {
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::MalformedCommand}),
                "ERR MALFORMED_COMMAND\n");
            EXPECT_EQ(
                encode_error(ProtocolError{
                    ProtocolErrorCode::LineTooLong}),
                "ERR LINE_TOO_LONG\n");
        }

        TEST(TradingLineProtocolEncoderTest,
             EncodesEveryBusinessResultWithRequestId) {
            const std::array<std::pair<TradingResult, std::string_view>, 10>
                cases{{
                    {TradingResult::Accepted, "ACCEPTED"},
                    {TradingResult::Cancelled, "CANCELLED"},
                    {TradingResult::AccountNotFound, "ACCOUNT_NOT_FOUND"},
                    {TradingResult::InsufficientFunds, "INSUFFICIENT_FUNDS"},
                    {TradingResult::DuplicateOrder, "DUPLICATE_ORDER"},
                    {TradingResult::InvalidOrder, "INVALID_ORDER"},
                    {TradingResult::CounterpartyNotAccountBacked,
                     "COUNTERPARTY_NOT_ACCOUNT_BACKED"},
                    {TradingResult::CancelNotFound, "CANCEL_NOT_FOUND"},
                    {TradingResult::CancelNotOwner, "CANCEL_NOT_OWNER"},
                    {TradingResult::InvalidRequest, "INVALID_REQUEST"},
                }};

            for (const auto& [result, expected_name] : cases) {
                const std::optional<OrderId> assigned_order_id =
                    result == TradingResult::Accepted
                    ? std::optional<OrderId>{77}
                    : std::nullopt;
                const std::string encoded = encode_trading_response(
                    TradingResponse{900, result, {}, assigned_order_id});
                const OrderId expected_order_id =
                    result == TradingResult::Accepted ? 77 : 0;
                EXPECT_EQ(
                    encoded,
                    "RESULT 900 " + std::string(expected_name) + " "
                        + std::to_string(expected_order_id) + " 0\n");

                const TradingResponseHeader header =
                    parse_trading_response_header(encoded);
                EXPECT_EQ(header.request_id, 900U);
                EXPECT_EQ(header.result, expected_name);
                EXPECT_EQ(header.assigned_order_id, expected_order_id);
                EXPECT_EQ(header.event_count, 0U);
            }
        }

        TEST(TradingLineProtocolEncoderTest,
             EncodesSuccessfulResponseEventsInOrder) {
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

            const std::string encoded = encode_trading_response(
                TradingResponse{901, TradingResult::Accepted, events, 1});

            EXPECT_EQ(
                encoded,
                "RESULT 901 ACCEPTED 1 5\n"
                "EVENT ORDER_ACCEPTED 1 BUY 100 7 10\n"
                "EVENT TRADE_CREATED 1 2 99 3 11\n"
                "EVENT ORDER_FILLED 2 SELL 3\n"
                "EVENT ORDER_PARTIALLY_FILLED 1 BUY 3 4\n"
                "EVENT ORDER_CANCELLED 1 BUY 100 4 10\n");
            const TradingResponseHeader header =
                parse_trading_response_header(encoded);
            EXPECT_EQ(header.request_id, 901U);
            EXPECT_EQ(header.result, "ACCEPTED");
            EXPECT_EQ(header.assigned_order_id, 1U);
            EXPECT_EQ(header.event_count, 5U);
        }

        TEST(TradingLineProtocolEncoderTest,
             FailureResponseNeverEncodesStaleEvents) {
            const std::vector<Event> stale_events{
                Event{EventPayload{OrderAccepted{
                    Order{1, Side::Buy, OrderType::Limit, 100, 7, 10}}}},
            };

            const std::string encoded = encode_trading_response(
                TradingResponse{
                    902,
                    TradingResult::InsufficientFunds,
                    stale_events,
                    99});

            EXPECT_EQ(encoded, "RESULT 902 INSUFFICIENT_FUNDS 0 0\n");
            EXPECT_EQ(encoded.find("EVENT "), std::string::npos);
        }

        TEST(TradingLineProtocolBoundaryTest,
             ProtocolErrorsRemainDistinctFromBusinessResults) {
            const TradingRequestParseResult parse_result =
                parse_trading_request("ADD 0 2 BUY 100 4");
            ASSERT_TRUE(std::holds_alternative<ProtocolError>(parse_result));

            const std::string business_response = encode_trading_response(
                TradingResponse{0, TradingResult::InvalidRequest, {}});
            EXPECT_EQ(business_response, "RESULT 0 INVALID_REQUEST 0 0\n");
            EXPECT_EQ(
                encode_error(std::get<ProtocolError>(parse_result)),
                "ERR MALFORMED_COMMAND\n");
        }
    }  // namespace
}  // namespace exchange
