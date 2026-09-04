#include "exchange/model_decision.hpp"

#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace exchange {
    namespace {
        TEST(ModelDecisionTest, ParsesStrictHold) {
            EXPECT_EQ(
                parse_model_decision(R"({"action":"hold"})"),
                ModelDecision{ModelHoldDecision{}});
        }

        TEST(ModelDecisionTest, ParsesStrictBuyAndSellSubmissions) {
            EXPECT_EQ(
                parse_model_decision(
                    R"({"action":"submit_order","side":"buy","price":95,"quantity":1})"),
                (ModelDecision{
                    ModelSubmitOrderDecision{Side::Buy, 95, 1}}));
            EXPECT_EQ(
                parse_model_decision(
                    R"({"quantity":2,"price":101,"side":"sell","action":"submit_order"})"),
                (ModelDecision{
                    ModelSubmitOrderDecision{Side::Sell, 101, 2}}));
        }

        TEST(ModelDecisionTest, ParsesStrictCancellation) {
            EXPECT_EQ(
                parse_model_decision(
                    R"({"action":"cancel_order","order_id":12})"),
                ModelDecision{ModelCancelOrderDecision{12}});
        }

        TEST(ModelDecisionTest, RejectsMalformedJsonAndUnknownActions) {
            EXPECT_THROW(
                static_cast<void>(parse_model_decision("not json")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"wait"})")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"(```json {"action":"hold"} ```)")),
                ModelDecisionParseError);
        }

        TEST(ModelDecisionTest, RejectsMissingAndUnexpectedFields) {
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"submit_order","side":"buy","price":95})")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"hold","reason":"safe"})")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"submit_order","side":"buy","price":95,"quantity":1,"account_id":999})")),
                ModelDecisionParseError);
        }

        TEST(ModelDecisionTest, RejectsWrongTypesAndInvalidSide) {
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"submit_order","side":"BUY","price":95,"quantity":1})")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"submit_order","side":"buy","price":"95","quantity":1})")),
                ModelDecisionParseError);
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(
                    R"({"action":"cancel_order","order_id":1.0})")),
                ModelDecisionParseError);
        }

        TEST(ModelDecisionTest, RejectsInvalidNumericValues) {
            for (const std::string& decision : {
                     R"({"action":"submit_order","side":"buy","price":0,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":-1,"quantity":1})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":0})",
                     R"({"action":"submit_order","side":"buy","price":1,"quantity":-1})",
                     R"({"action":"cancel_order","order_id":0})",
                     R"({"action":"cancel_order","order_id":-1})"}) {
                EXPECT_THROW(
                    static_cast<void>(parse_model_decision(decision)),
                    ModelDecisionParseError);
            }

            const std::string out_of_range =
                R"({"action":"submit_order","side":"buy","price":)"
                + std::to_string(
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    + 1)
                + R"(,"quantity":1})";
            EXPECT_THROW(
                static_cast<void>(parse_model_decision(out_of_range)),
                ModelDecisionParseError);
        }
    }  // namespace
}  // namespace exchange
