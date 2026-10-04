#include "diagnostics/partial_fill_evidence.hpp"

#include <stdexcept>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace exchange::diagnostics {
    namespace {
        TEST(PartialFillEvidenceTest,
             PartialFillEvidenceContainsRequiredFacts) {
            const PartialFillEvidence evidence = make_partial_fill_evidence();

            EXPECT_EQ(evidence.request_id, 1042U);
            EXPECT_EQ(evidence.account_id, 1U);
            EXPECT_EQ(evidence.submitted_order.id, 3U);
            EXPECT_EQ(evidence.submitted_order.side, Side::Buy);
            EXPECT_EQ(evidence.submitted_order.price, 100);
            EXPECT_EQ(evidence.submitted_order.quantity, 5);
            EXPECT_EQ(evidence.submitted_order.timestamp, 3);
            EXPECT_EQ(evidence.wal_sequence, 3U);
            EXPECT_EQ(evidence.execution_result, TradingResult::Accepted);
            ASSERT_EQ(evidence.trades.size(), 1U);
            EXPECT_EQ(evidence.trades[0].trade.buy_order_id, 3U);
            EXPECT_EQ(evidence.trades[0].trade.sell_order_id, 1U);
            EXPECT_EQ(evidence.trades[0].trade.price, 100);
            EXPECT_EQ(evidence.trades[0].trade.quantity, 2);
            EXPECT_EQ(evidence.trades[0].ledger_sequence, 4U);
            ASSERT_TRUE(evidence.resting_order_after.has_value());
            EXPECT_EQ(evidence.resting_order_after->quantity, 3);
            EXPECT_EQ(evidence.resting_order_after->price, 100);
            ASSERT_TRUE(evidence.reservation_after.has_value());
            EXPECT_EQ(evidence.reservation_after->remaining_amount, 300);

            const nlohmann::json json = nlohmann::json::parse(
                partial_fill_evidence_json(evidence));
            EXPECT_EQ(json.at("case_type"), "partial_fill");
            EXPECT_EQ(json.at("target_order").at("order_id"), 3);
            EXPECT_EQ(json.at("provenance").at("request_id"), 1042);
            EXPECT_EQ(json.at("provenance").at("wal_sequence"), 3);
            EXPECT_EQ(json.at("trades").at(0).at("ledger_sequence"), 4);
            EXPECT_EQ(json.at("execution").at("events").size(),
                      evidence.execution_events.size());
            EXPECT_EQ(json.at("execution").at("events").at(0).at("type"),
                      "ORDER_ACCEPTED");
            EXPECT_EQ(json.at("execution").at("events").at(1).at("type"),
                      "TRADE_CREATED");
            EXPECT_EQ(json.at("execution").at("events").at(3).at("type"),
                      "ORDER_PARTIALLY_FILLED");
            EXPECT_EQ(json.at("execution").at("events").at(3)
                          .at("remaining_quantity"), 3);
        }

        TEST(PartialFillEvidenceTest,
             PartialFillEvidenceIsInternallyConsistent) {
            PartialFillEvidence evidence = make_partial_fill_evidence();

            EXPECT_NO_THROW(validate_partial_fill_evidence(evidence));
            EXPECT_EQ(evidence.matched_quantity, 2);
            ASSERT_TRUE(evidence.resting_order_after.has_value());
            EXPECT_EQ(evidence.submitted_order.quantity,
                      evidence.matched_quantity
                          + evidence.resting_order_after->quantity);
            EXPECT_EQ(evidence.total_executable_quantity_before,
                      evidence.matched_quantity);
            EXPECT_EQ(partial_fill_evidence_json(evidence),
                      partial_fill_evidence_json(
                          make_partial_fill_evidence()));

            ++evidence.matched_quantity;
            EXPECT_THROW(validate_partial_fill_evidence(evidence),
                         std::logic_error);
        }

        TEST(PartialFillEvidenceTest,
             PartialFillEvidenceDistinguishesExecutableAndNonExecutableLiquidity) {
            const PartialFillEvidence evidence = make_partial_fill_evidence();

            ASSERT_EQ(evidence.opposing_orders_before.size(), 2U);
            EXPECT_EQ(evidence.opposing_orders_before[0].order.id, 1U);
            EXPECT_EQ(evidence.opposing_orders_before[0].order.price, 100);
            EXPECT_EQ(evidence.opposing_orders_before[0].order.quantity, 2);
            EXPECT_TRUE(evidence.opposing_orders_before[0].executable);
            EXPECT_EQ(evidence.opposing_orders_before[1].order.id, 2U);
            EXPECT_EQ(evidence.opposing_orders_before[1].order.price, 101);
            EXPECT_EQ(evidence.opposing_orders_before[1].order.quantity, 4);
            EXPECT_FALSE(evidence.opposing_orders_before[1].executable);
            EXPECT_EQ(evidence.total_executable_quantity_before, 2);
            EXPECT_EQ(evidence.best_ask_after, 101);

            const nlohmann::json json = nlohmann::json::parse(
                partial_fill_evidence_json(evidence));
            const auto& opposing = json.at("pre_execution_book")
                                      .at("opposing_orders");
            EXPECT_EQ(opposing.at(0).at("executable"), true);
            EXPECT_EQ(opposing.at(1).at("executable"), false);
            EXPECT_EQ(json.at("post_execution").at("best_ask"), 101);
        }
    }  // namespace
}  // namespace exchange::diagnostics
