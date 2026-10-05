#include "diagnostics/historical_order_evidence.hpp"
#include "execution/trading_runtime.hpp"
#include "../../src/diagnostics/historical_order_outcomes.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace exchange::diagnostics {
    namespace {
        HistoricalEvidenceConfig config() {
            return {{20, 10, 1, 1, 1}, {{
                BootstrapAccount{11, {{10, {1'000, 0}}}},
                BootstrapAccount{22, {{20, {10, 0}}}}}}};
        }

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern = (std::filesystem::temp_directory_path()
                    / "exchange-historical-test-XXXXXX").string();
                const char* created = ::mkdtemp(pattern.data());
                if (!created) {
                    throw std::system_error(errno, std::generic_category(), "mkdtemp");
                }
                path_ = created;
            }
            ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
            std::string file(const char* name) const { return path_ + "/" + name; }

        private:
            std::string path_;
        };

        TradingRequest submit(RequestId request, AccountId account,
                              Side side, Quantity quantity) {
            return {request, account, SubmitTradingRequest{side, 100, quantity}};
        }

        void create_workload(const std::string& path) {
            const auto settings = config();
            const auto runtime = TradingRuntime::create_durable(
                settings.instrument, path, settings.bootstrap);
            const auto first = runtime->executor().execute(submit(1001, 22, Side::Sell, 2));
            const auto second = runtime->executor().execute(submit(2002, 11, Side::Buy, 5));
            // A durable cancel between submits separates WalSequence/OrderId.
            const auto cancel = runtime->executor().execute(
                {3003, 11, CancelTradingRequest{9999}});
            const auto third = runtime->executor().execute(submit(4004, 22, Side::Sell, 1));
            if (first.result != TradingResult::Accepted || first.assigned_order_id != 1
                || second.result != TradingResult::Accepted || second.assigned_order_id != 2
                || cancel.result != TradingResult::CancelNotFound
                || third.result != TradingResult::Accepted || third.assigned_order_id != 3) {
                throw std::logic_error("historical fixture execution failed");
            }
            // No responses, events, or runtime pointer escape this function.
        }

        std::string read_file(const std::string& path) {
            std::ifstream input(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }

        void write_file(const std::string& path, const std::string& bytes) {
            std::ofstream output(path, std::ios::binary);
            output << bytes;
            if (!output) {
                throw std::runtime_error("cannot write test file");
            }
        }

        nlohmann::json config_document() {
            return {
                {"instrument", {
                    {"base_asset", 20}, {"quote_asset", 10},
                    {"base_atomic_units_per_quantity_unit", 1},
                    {"quote_atomic_units_per_price_quantity_numerator", 1},
                    {"quote_atomic_units_per_price_quantity_denominator", 1}}},
                {"bootstrap", { {"accounts", nlohmann::json::array({
                    {{"account_id", 11}, {"balances", nlohmann::json::array({
                        {{"asset_id", 10}, {"balance", {{"available", 1000}, {"reserved", 0}}}}})}},
                    {{"account_id", 22}, {"balances", nlohmann::json::array({
                        {{"asset_id", 20}, {"balance", {{"available", 10}, {"reserved", 0}}}}})}}
                })}}}};
        }

        class HistoricalOrderEvidenceTest : public ::testing::Test {
        protected:
            void SetUp() override { create_workload(wal_path()); }
            std::string wal_path() const { return directory.file("execution.wal"); }
            auto lookup(OrderId id) const {
                return reconstruct_historical_order_evidence(config(), wal_path(), id);
            }
            TemporaryDirectory directory;
        };

        TEST_F(HistoricalOrderEvidenceTest, SubmissionUsesRecordedOrderIdNotOtherIdentifiers) {
            const auto third = lookup(3);
            ASSERT_TRUE(third);
            EXPECT_EQ(third->submission.order.id, 3U);
            EXPECT_EQ(third->submission.request_id, 4004U);
            EXPECT_EQ(third->submission.account_id, 22U);
            EXPECT_EQ(third->submission.order.timestamp, 3);
            EXPECT_EQ(third->submission_wal_sequence, 4U);
            EXPECT_EQ(third->submission_recovered_result, TradingResult::Accepted);
            EXPECT_TRUE(third->cancel_attempts.empty());
            EXPECT_EQ(third->submission.order.quantity, 1);
            EXPECT_EQ(third->submission.order.price, 100);
            EXPECT_EQ(third->submission.order.side, Side::Sell);
            const auto first = lookup(1);
            ASSERT_TRUE(first);
            EXPECT_EQ(first->submission.request_id, 1001U);
            EXPECT_EQ(first->submission_wal_sequence, 1U);
        }

        TEST_F(HistoricalOrderEvidenceTest, RestingOrderIncludesAllLaterLedgerTradesAndReservation) {
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::Resting);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            EXPECT_EQ(evidence->matched_quantity, 3);
            ASSERT_EQ(evidence->trades.size(), 2U);
            EXPECT_EQ(evidence->trades[0].trade.sell_order_id, 1U);
            EXPECT_EQ(evidence->trades[0].trade.quantity, 2);
            EXPECT_EQ(evidence->trades[0].ledger_sequence, 3U);
            EXPECT_EQ(evidence->trades[1].trade.sell_order_id, 3U);
            EXPECT_EQ(evidence->trades[1].trade.quantity, 1);
            EXPECT_EQ(evidence->trades[1].trade.timestamp, 3);
            EXPECT_EQ(evidence->trades[1].ledger_sequence, 5U);
            ASSERT_TRUE(evidence->resting_order);
            EXPECT_EQ(evidence->resting_order->quantity, 2);
            EXPECT_EQ(evidence->matched_quantity + evidence->resting_order->quantity,
                      evidence->submission.order.quantity);
            ASSERT_TRUE(evidence->reservation);
            EXPECT_EQ(evidence->reservation->remaining_amount, 200);
            EXPECT_EQ(evidence->recovered_through_wal_sequence, 4U);
        }

        TEST_F(HistoricalOrderEvidenceTest, FullyFilledRequiresCompleteTradeSumAndNoRestingOrder) {
            const auto evidence = lookup(1);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::FullyFilled);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            EXPECT_EQ(evidence->matched_quantity, evidence->submission.order.quantity);
            ASSERT_EQ(evidence->trades.size(), 1U);
            EXPECT_EQ(evidence->trades[0].trade.buy_order_id, 2U);
            EXPECT_FALSE(evidence->resting_order);
            EXPECT_FALSE(evidence->reservation);
            const auto json = nlohmann::json::parse(historical_order_evidence_json(*evidence));
            EXPECT_EQ(json["final_state"]["status"], "fully_filled");
            EXPECT_EQ(json["submission"]["recovered_result"], "Accepted");
            EXPECT_EQ(json["final_state"]["remaining_quantity"], 0);
            EXPECT_TRUE(json["final_state"]["reservation"].is_null());
        }

        TEST_F(HistoricalOrderEvidenceTest, UnknownOrderDoesNotSubstituteRequestOrCancelTarget) {
            for (OrderId id : {4U, 4004U, 9999U}) {
                EXPECT_FALSE(lookup(id));
            }
            EXPECT_THROW(lookup(0), std::invalid_argument);
        }

        TEST_F(HistoricalOrderEvidenceTest, JsonIsDeterministicAndMarksMissingContextExplicitly) {
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            const auto text = historical_order_evidence_json(*evidence);
            EXPECT_EQ(text, historical_order_evidence_json(*lookup(2)));
            const auto json = nlohmann::json::parse(text);
            EXPECT_EQ(json["case_type"], "historical_durable_order");
            EXPECT_EQ(json["order_id"], 2);
            EXPECT_EQ(json["submission"]["request_id"], 2002);
            EXPECT_EQ(json["submission"]["recovered_result"], "Accepted");
            EXPECT_EQ(json["outcome_basis"], "deterministic_replay");
            EXPECT_EQ(json["cancel_attempts"], nlohmann::json::array());
            EXPECT_EQ(json["final_state"]["status"], "resting");
            EXPECT_EQ(json["final_state"]["remaining_quantity"], 2);
            EXPECT_EQ(json["final_state"]["price"], 100);
            EXPECT_EQ(json["final_state"]["side"], "BUY");
            EXPECT_EQ(json["evidence_scope"], nlohmann::json({
                "durable_submission", "recovered_ledger", "recovered_final_state",
                "recovered_trading_outcomes"}));
            for (const auto& value : json["unavailable_context"]) {
                EXPECT_TRUE(value.is_null());
            }
            EXPECT_FALSE(json.contains("cause"));
            EXPECT_FALSE(json.contains("execution"));
            EXPECT_EQ(read_file(wal_path()).size(),
                      kWalFileHeaderEncodedSize + 3 * kWalSubmitRecordEncodedSize
                          + kWalCancelRecordEncodedSize);
        }

        TEST_F(HistoricalOrderEvidenceTest, CancelledOrderIsConservativelyNotResting) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                EXPECT_EQ(runtime->executor().execute(
                    {5050, 11, CancelTradingRequest{2}}).result, TradingResult::Cancelled);
            }
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->matched_quantity, 3);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::NotResting);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            const auto& attempt = evidence->cancel_attempts.front();
            EXPECT_EQ(attempt.command.request_id, 5050U);
            EXPECT_EQ(attempt.command.account_id, 11U);
            EXPECT_EQ(attempt.command.order_id, 2U);
            EXPECT_EQ(attempt.wal_sequence, 5U);
            EXPECT_EQ(attempt.recovered_result, TradingResult::Cancelled);
            const auto json = nlohmann::json::parse(historical_order_evidence_json(*evidence));
            EXPECT_EQ(json["cancel_attempts"][0], (nlohmann::json{
                {"request_id", 5050}, {"account_id", 11}, {"order_id", 2},
                {"wal_sequence", 5}, {"recovered_result", "Cancelled"}}));
            EXPECT_TRUE(json["final_state"]["remaining_quantity"].is_null());
            EXPECT_EQ(json["final_state"]["status"], "not_resting");
        }

        TEST_F(HistoricalOrderEvidenceTest, DurableBusinessRejectionDoesNotClaimAcceptanceOrFill) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                EXPECT_EQ(runtime->executor().execute(
                    submit(6060, 11, Side::Buy, 100)).result, TradingResult::InsufficientFunds);
                // Static invalidity is rejected before WAL admission.
                EXPECT_EQ(runtime->executor().execute(
                    submit(7070, 11, Side::Buy, 0)).result, TradingResult::InvalidOrder);
                EXPECT_EQ(runtime->executor().execute(
                    {8080, 11, CancelTradingRequest{4}}).result, TradingResult::CancelNotFound);
            }
            const auto evidence = lookup(4);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission.request_id, 6060U);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::InsufficientFunds);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::NotResting);
            EXPECT_TRUE(evidence->trades.empty());
            EXPECT_EQ(evidence->matched_quantity, 0);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].wal_sequence, 6U);
            EXPECT_EQ(evidence->cancel_attempts[0].recovered_result, TradingResult::CancelNotFound);
            const auto json = nlohmann::json::parse(historical_order_evidence_json(*evidence));
            EXPECT_EQ(json["submission"]["recovered_result"], "InsufficientFunds");
            EXPECT_EQ(json["cancel_attempts"][0]["recovered_result"], "CancelNotFound");
            EXPECT_EQ(json["final_state"]["status"], "not_resting");
            EXPECT_TRUE(json["final_state"]["remaining_quantity"].is_null());
            EXPECT_TRUE(json["unavailable_context"]["response_delivery"].is_null());
            EXPECT_FALSE(lookup(5));
        }

        TEST_F(HistoricalOrderEvidenceTest, NonexistentAccountSubmissionExposesRecoveredRejection) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                ASSERT_EQ(runtime->executor().execute(
                    submit(6060, 999, Side::Buy, 1)).result, TradingResult::AccountNotFound);
            }
            const auto evidence = lookup(4);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission.account_id, 999U);
            EXPECT_EQ(evidence->submission_wal_sequence, 5U);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::AccountNotFound);
            EXPECT_TRUE(evidence->trades.empty());
            EXPECT_TRUE(evidence->cancel_attempts.empty());
            EXPECT_EQ(evidence->matched_quantity, 0);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::NotResting);
            const auto json = nlohmann::json::parse(historical_order_evidence_json(*evidence));
            EXPECT_EQ(json["submission"]["recovered_result"], "AccountNotFound");
        }

        TEST_F(HistoricalOrderEvidenceTest, WrongOwnerCancellationLeavesOrderResting) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                ASSERT_EQ(runtime->executor().execute(
                    {5050, 22, CancelTradingRequest{2}}).result, TradingResult::CancelNotOwner);
            }
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].recovered_result, TradingResult::CancelNotOwner);
            EXPECT_EQ(evidence->cancel_attempts[0].command.account_id, 22U);
            EXPECT_EQ(evidence->cancel_attempts[0].wal_sequence, 5U);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::Resting);
            ASSERT_TRUE(evidence->resting_order);
            EXPECT_EQ(evidence->resting_order->quantity, 2);
            ASSERT_TRUE(evidence->reservation);
            EXPECT_EQ(evidence->reservation->remaining_amount, 200);
            EXPECT_EQ(evidence->matched_quantity, 3);
            const auto json = nlohmann::json::parse(historical_order_evidence_json(*evidence));
            EXPECT_EQ(json["cancel_attempts"][0]["recovered_result"], "CancelNotOwner");
        }

        TEST_F(HistoricalOrderEvidenceTest, LaterFillsAndRepeatedRequestIdCancellationsRemainDistinct) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                // All attempts reuse the submission RequestId, while owners
                // and WAL sequences differ. RequestId does not join records.
                ASSERT_EQ(runtime->executor().execute(
                    {2002, 22, CancelTradingRequest{2}}).result, TradingResult::CancelNotOwner);
                ASSERT_EQ(runtime->executor().execute(
                    {2002, 11, CancelTradingRequest{2}}).result, TradingResult::Cancelled);
                ASSERT_EQ(runtime->executor().execute(
                    {2002, 11, CancelTradingRequest{2}}).result, TradingResult::CancelNotFound);
            }
            const auto original = read_file(wal_path());
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission.request_id, 2002U);
            EXPECT_EQ(evidence->submission_wal_sequence, 2U);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            ASSERT_EQ(evidence->trades.size(), 2U);
            EXPECT_EQ(evidence->trades[0].trade.quantity, 2);
            EXPECT_EQ(evidence->trades[1].trade.quantity, 1);
            EXPECT_EQ(evidence->matched_quantity, 3);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::NotResting);
            EXPECT_FALSE(evidence->reservation);
            EXPECT_FALSE(evidence->resting_order);
            ASSERT_EQ(evidence->cancel_attempts.size(), 3U);
            const std::vector<TradingResult> expected{
                TradingResult::CancelNotOwner, TradingResult::Cancelled, TradingResult::CancelNotFound};
            for (std::size_t index = 0; index < expected.size(); ++index) {
                const auto& attempt = evidence->cancel_attempts[index];
                EXPECT_EQ(attempt.command.request_id, 2002U);
                EXPECT_EQ(attempt.command.order_id, 2U);
                EXPECT_EQ(attempt.wal_sequence, 5 + index);
                EXPECT_EQ(attempt.recovered_result, expected[index]);
            }
            const auto text = historical_order_evidence_json(*evidence);
            EXPECT_EQ(text, historical_order_evidence_json(*lookup(2)));
            const auto json = nlohmann::json::parse(text);
            EXPECT_EQ(json["cancel_attempts"], (nlohmann::json::array({
                {{"request_id", 2002}, {"account_id", 22}, {"order_id", 2},
                    {"wal_sequence", 5}, {"recovered_result", "CancelNotOwner"}},
                {{"request_id", 2002}, {"account_id", 11}, {"order_id", 2},
                    {"wal_sequence", 6}, {"recovered_result", "Cancelled"}},
                {{"request_id", 2002}, {"account_id", 11}, {"order_id", 2},
                    {"wal_sequence", 7}, {"recovered_result", "CancelNotFound"}}
            })));
            EXPECT_EQ(json["recovery"]["through_wal_sequence"], 7);
            for (const auto& value : json["unavailable_context"]) {
                EXPECT_TRUE(value.is_null());
            }
            EXPECT_EQ(read_file(wal_path()), original);
        }

        TEST_F(HistoricalOrderEvidenceTest, CancellationAfterFullFillIsAnAttemptWithoutChangingFinalState) {
            const auto settings = config();
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, wal_path(), settings.bootstrap);
                ASSERT_EQ(runtime->executor().execute(
                    {5050, 22, CancelTradingRequest{1}}).result, TradingResult::CancelNotFound);
            }
            const auto evidence = lookup(1);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::FullyFilled);
            EXPECT_EQ(evidence->matched_quantity, 2);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].recovered_result, TradingResult::CancelNotFound);
        }

        TEST(HistoricalOrderLifecycleTest, CancellationBeforeSubmissionIsRetainedByTargetAndWalSequence) {
            TemporaryDirectory directory;
            const auto settings = config();
            const auto path = directory.file("execution.wal");
            {
                const auto runtime = TradingRuntime::create_durable(
                    settings.instrument, path, settings.bootstrap);
                ASSERT_EQ(runtime->executor().execute(
                    {42, 11, CancelTradingRequest{1}}).result, TradingResult::CancelNotFound);
                ASSERT_EQ(runtime->executor().execute(
                    submit(42, 11, Side::Buy, 1)).result, TradingResult::Accepted);
            }
            const auto evidence = reconstruct_historical_order_evidence(settings, path, 1);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission_wal_sequence, 2U);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].wal_sequence, 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].recovered_result, TradingResult::CancelNotFound);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::Resting);
            EXPECT_FALSE(reconstruct_historical_order_evidence(settings, path, 42));
        }

        std::vector<WalRecord> association_records() {
            return {
                {1, SubmitExecutionCommand{
                    42, 11, {1, Side::Buy, OrderType::Limit, 100, 1, 1}}},
                {2, CancelExecutionCommand{42, 11, 1}}};
        }

        TEST(HistoricalOrderOutcomeAssociationTest, MissingSubmitOrCancelOutcomeFailsConstruction) {
            const auto records = association_records();
            for (const auto& outcomes : std::vector<std::vector<RecoveredTradingOutcome>>{
                    {}, {{2, TradingResult::Cancelled}}, {{1, TradingResult::Accepted}}}) {
                EXPECT_THROW(static_cast<void>(detail::associate_recovered_order_outcomes(
                    1, records, outcomes)), std::logic_error);
            }
        }

        TEST(HistoricalOrderOutcomeAssociationTest, DuplicateUnmatchedAndOutOfOrderOutcomesFailConstruction) {
            const auto records = association_records();
            for (const auto& outcomes : std::vector<std::vector<RecoveredTradingOutcome>>{
                    {{1, TradingResult::Accepted}, {1, TradingResult::Accepted},
                        {2, TradingResult::Cancelled}},
                    {{1, TradingResult::Accepted}, {2, TradingResult::Cancelled},
                        {2, TradingResult::Cancelled}},
                    {{1, TradingResult::Accepted}, {2, TradingResult::Cancelled},
                        {3, TradingResult::CancelNotFound}},
                    {{2, TradingResult::Cancelled}, {1, TradingResult::Accepted}}}) {
                EXPECT_THROW(static_cast<void>(detail::associate_recovered_order_outcomes(
                    1, records, outcomes)), std::logic_error);
            }
        }

        TEST(HistoricalOrderOutcomeAssociationTest, ResultsMustMatchTradingCommandType) {
            const auto records = association_records();
            for (const auto& outcomes : std::vector<std::vector<RecoveredTradingOutcome>>{
                    {{1, TradingResult::Cancelled}, {2, TradingResult::Cancelled}},
                    {{1, TradingResult::Accepted}, {2, TradingResult::Accepted}},
                    {{1, TradingResult::InvalidRequest}, {2, TradingResult::Cancelled}},
                    {{1, static_cast<TradingResult>(999)}, {2, TradingResult::Cancelled}}}) {
                EXPECT_THROW(static_cast<void>(detail::associate_recovered_order_outcomes(
                    1, records, outcomes)), std::logic_error);
            }
        }

        TEST(HistoricalOrderOutcomeAssociationTest, ContractRecordsDoNotConsumeTradingOutcomes) {
            const std::vector<WalRecord> records{
                {1, CreateContractExecutionCommand{1, 101, 202,
                    {101, 202, 100, ResourceKind::ComputeCredit, 1}}},
                {2, SubmitExecutionCommand{
                    42, 11, {1, Side::Buy, OrderType::Limit, 100, 1, 1}}},
                {3, AcceptContractExecutionCommand{1, 202}},
                {4, CancelExecutionCommand{42, 11, 1}}};
            const std::vector<RecoveredTradingOutcome> outcomes{
                {2, TradingResult::Accepted}, {4, TradingResult::Cancelled}};
            const auto evidence = detail::associate_recovered_order_outcomes(1, records, outcomes);
            ASSERT_TRUE(evidence);
            EXPECT_EQ(evidence->submission_wal_sequence, 2U);
            ASSERT_EQ(evidence->cancel_attempts.size(), 1U);
            EXPECT_EQ(evidence->cancel_attempts[0].wal_sequence, 4U);
            auto invalid = outcomes;
            invalid.insert(invalid.begin(), {1, TradingResult::Accepted});
            EXPECT_THROW(static_cast<void>(detail::associate_recovered_order_outcomes(
                1, records, invalid)), std::logic_error);
        }

        TEST_F(HistoricalOrderEvidenceTest, ResultSerializationUsesExactTradingResultNames) {
            const auto recovered = lookup(2);
            ASSERT_TRUE(recovered);
            for (const auto& [result, name] : std::vector<std::pair<TradingResult, std::string>>{
                    {TradingResult::Accepted, "Accepted"},
                    {TradingResult::Cancelled, "Cancelled"},
                    {TradingResult::AccountNotFound, "AccountNotFound"},
                    {TradingResult::InsufficientFunds, "InsufficientFunds"},
                    {TradingResult::DuplicateOrder, "DuplicateOrder"},
                    {TradingResult::InvalidOrder, "InvalidOrder"},
                    {TradingResult::CounterpartyNotAccountBacked, "CounterpartyNotAccountBacked"},
                    {TradingResult::CancelNotFound, "CancelNotFound"},
                    {TradingResult::CancelNotOwner, "CancelNotOwner"},
                    {TradingResult::InvalidRequest, "InvalidRequest"}}) {
                // Isolate serialization from which values are applicable to a
                // submit versus cancel; association tests enforce that rule.
                auto evidence = *recovered;
                evidence.submission_recovered_result = result;
                evidence.cancel_attempts = {{{42, 11, 2}, 5, result}};
                const auto json = nlohmann::json::parse(historical_order_evidence_json(evidence));
                EXPECT_EQ(json["submission"]["recovered_result"], name);
                EXPECT_EQ(json["cancel_attempts"][0]["recovered_result"], name);
            }
            auto invalid = *recovered;
            invalid.submission_recovered_result = static_cast<TradingResult>(999);
            EXPECT_THROW(historical_order_evidence_json(invalid), std::logic_error);
        }

        TEST_F(HistoricalOrderEvidenceTest, OfflineSourceBytesAreUnchangedIncludingTornTail) {
            const auto original = read_file(wal_path());
            ASSERT_TRUE(lookup(2));
            EXPECT_EQ(read_file(wal_path()), original);
            // Copy an incomplete next record; production recovery discards it.
            const auto encoded = encode_wal_record(
                {5, CancelExecutionCommand{5050, 11, 2}}, config().instrument);
            ASSERT_TRUE(std::holds_alternative<WalBytes>(encoded));
            const auto& bytes = std::get<WalBytes>(encoded);
            const auto torn = original + std::string(
                reinterpret_cast<const char*>(bytes.data()), bytes.size() / 2);
            write_file(wal_path(), torn);
            const auto evidence = lookup(2);
            ASSERT_TRUE(evidence);
            EXPECT_TRUE(evidence->ignored_torn_tail);
            EXPECT_EQ(evidence->recovered_through_wal_sequence, 4U);
            EXPECT_EQ(evidence->final_state, HistoricalOrderState::Resting);
            EXPECT_EQ(evidence->submission_recovered_result, TradingResult::Accepted);
            EXPECT_TRUE(evidence->cancel_attempts.empty());
            EXPECT_EQ(read_file(wal_path()), torn);
        }

        TEST_F(HistoricalOrderEvidenceTest, ActiveWriterIsRejectedWithoutMutation) {
            const auto settings = config();
            const auto runtime = TradingRuntime::create_durable(
                settings.instrument, wal_path(), settings.bootstrap);
            const auto original = read_file(wal_path());
            EXPECT_THROW(lookup(2), std::system_error);
            EXPECT_EQ(read_file(wal_path()), original);
        }

        TEST_F(HistoricalOrderEvidenceTest, MissingEmptyCorruptAndMismatchedWalFailSafely) {
            EXPECT_THROW(reconstruct_historical_order_evidence(
                config(), directory.file("missing.wal"), 2), std::system_error);
            EXPECT_FALSE(std::filesystem::exists(directory.file("missing.wal")));
            auto wrong = config();
            ++wrong.bootstrap.accounts[0].balances[0].balance.available;
            EXPECT_THROW(reconstruct_historical_order_evidence(wrong, wal_path(), 2), std::runtime_error);
            wrong = config();
            ++wrong.instrument.base_asset;
            EXPECT_THROW(reconstruct_historical_order_evidence(wrong, wal_path(), 2), std::runtime_error);
            auto bytes = read_file(wal_path());
            bytes[kWalFileHeaderEncodedSize + 20] ^= 1;
            write_file(wal_path(), bytes);
            EXPECT_THROW(lookup(2), std::runtime_error);
            EXPECT_EQ(read_file(wal_path()), bytes);
            write_file(wal_path(), "");
            EXPECT_THROW(lookup(2), std::runtime_error);
            EXPECT_TRUE(read_file(wal_path()).empty());
        }

        TEST_F(HistoricalOrderEvidenceTest, ConfigUsesProductionTypesAndRejectsNumericCoercion) {
            const auto path = directory.file("config.json");
            write_file(path, config_document().dump());
            const auto loaded = load_historical_evidence_config(path);
            EXPECT_EQ(loaded.instrument, config().instrument);
            EXPECT_EQ(loaded.bootstrap, config().bootstrap);
            for (const auto& bad : std::vector<nlohmann::json>{true, 1.0, -1, "1", 0}) {
                auto json = config_document();
                json["instrument"]["base_asset"] = bad;
                write_file(path, json.dump());
                EXPECT_THROW(load_historical_evidence_config(path), std::exception);
            }
            auto json = config_document();
            json["bootstrap"]["accounts"][0]["balances"][0]["balance"]["available"]
                = std::numeric_limits<std::uint64_t>::max();
            write_file(path, json.dump());
            EXPECT_THROW(load_historical_evidence_config(path), std::invalid_argument);
            write_file(path, "{");
            EXPECT_THROW(load_historical_evidence_config(path), std::exception);
        }

        int wait_child(pid_t child) {
            int status = 0;
            pid_t waited;
            do {
                waited = ::waitpid(child, &status, 0);
            } while (waited == -1 && errno == EINTR);
            if (waited != child || !WIFEXITED(status)) {
                return -1;
            }
            return WEXITSTATUS(status);
        }

        int run_cli(const TemporaryDirectory& directory, const std::vector<std::string>& arguments) {
            const pid_t child = ::fork();
            if (child == -1) {
                throw std::system_error(errno, std::generic_category(), "fork CLI");
            }
            if (child == 0) {
                ::alarm(10);
                const int output = ::open(directory.file("output.json").c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
                const int error = ::open("/dev/null", O_WRONLY);
                if (output < 0 || error < 0 || ::dup2(output, STDOUT_FILENO) < 0
                    || ::dup2(error, STDERR_FILENO) < 0) {
                    _exit(120);
                }
                ::close(output);
                ::close(error);
                std::vector<char*> argv{const_cast<char*>(EXCHANGE_HISTORICAL_EVIDENCE_PATH)};
                for (const auto& argument : arguments) {
                    argv.push_back(const_cast<char*>(argument.c_str()));
                }
                argv.push_back(nullptr);
                ::execv(argv[0], argv.data());
                _exit(121);
            }
            return wait_child(child);
        }

        TEST(HistoricalOrderEvidenceProcessTest, ProducerExitsBeforeStandaloneRecoveryQueries) {
            TemporaryDirectory directory;
            const auto wal = directory.file("execution.wal");
            const pid_t producer = ::fork();
            ASSERT_NE(producer, -1);
            if (producer == 0) {
                ::alarm(10);
                try {
                    create_workload(wal);
                    _exit(0);
                } catch (...) {
                    _exit(122);
                }
            }
            ASSERT_EQ(wait_child(producer), 0);
            const auto original = read_file(wal);
            const auto path = directory.file("config.json");
            write_file(path, config_document().dump());
            for (const auto& [id, state] : std::vector<std::pair<std::string, std::string>>{
                     {"2", "resting"}, {"1", "fully_filled"}, {"3", "fully_filled"}}) {
                ASSERT_EQ(run_cli(directory, {"--config", path, "--wal", wal, "--order-id", id}), 0);
                const auto json = nlohmann::json::parse(read_file(directory.file("output.json")));
                EXPECT_EQ(json["order_id"], std::stoul(id));
                EXPECT_EQ(json["final_state"]["status"], state);
                EXPECT_EQ(json["recovery"]["through_wal_sequence"], 4);
                EXPECT_EQ(json["submission"]["recovered_result"], "Accepted");
                EXPECT_EQ(json["outcome_basis"], "deterministic_replay");
                EXPECT_EQ(json["cancel_attempts"], nlohmann::json::array());
                EXPECT_EQ(read_file(wal), original);
            }
            EXPECT_EQ(run_cli(directory, {"--wal", wal, "--order-id", "999", "--config", path}), 2);
            EXPECT_EQ(nlohmann::json::parse(read_file(directory.file("output.json")))["error"],
                      "order_not_found");
            EXPECT_EQ(run_cli(directory, {"--wal", directory.file("missing"), "--order-id", "2", "--config", path}), 1);
            for (const auto& id : {"0", "-1", "2junk", "18446744073709551616"}) {
                EXPECT_EQ(run_cli(directory, {"--wal", wal, "--order-id", id, "--config", path}), 1);
            }
            EXPECT_EQ(run_cli(directory, {}), 1);
            EXPECT_EQ(run_cli(directory, {"--wal", wal, "--wal", wal, "--order-id", "2"}), 1);
            write_file(wal, "corrupt WAL");
            EXPECT_EQ(run_cli(directory, {"--wal", wal, "--order-id", "2", "--config", path}), 1);
            write_file(path, "{}");
            EXPECT_EQ(run_cli(directory, {"--wal", wal, "--order-id", "2", "--config", path}), 1);
        }

        TEST(HistoricalOrderEvidenceProcessTest, StandaloneCliReconstructsDurableCancelAttemptsAfterProducerExit) {
            TemporaryDirectory directory;
            const auto wal = directory.file("execution.wal");
            const pid_t producer = ::fork();
            ASSERT_NE(producer, -1);
            if (producer == 0) {
                ::alarm(10);
                try {
                    create_workload(wal);
                    const auto settings = config();
                    const auto runtime = TradingRuntime::create_durable(
                        settings.instrument, wal, settings.bootstrap);
                    if (runtime->executor().execute({2002, 22, CancelTradingRequest{2}}).result
                            != TradingResult::CancelNotOwner
                        || runtime->executor().execute({2002, 11, CancelTradingRequest{2}}).result
                            != TradingResult::Cancelled
                        || runtime->executor().execute({2002, 11, CancelTradingRequest{2}}).result
                            != TradingResult::CancelNotFound) {
                        _exit(123);
                    }
                } catch (...) {
                    _exit(122);
                }
                _exit(0);
            }
            ASSERT_EQ(wait_child(producer), 0);
            const auto original = read_file(wal);
            const auto path = directory.file("config.json");
            write_file(path, config_document().dump());
            const std::vector<std::string> arguments{
                "--wal", wal, "--order-id", "2", "--config", path};
            ASSERT_EQ(run_cli(directory, arguments), 0);
            const auto text = read_file(directory.file("output.json"));
            const auto json = nlohmann::json::parse(text);
            EXPECT_EQ(json["outcome_basis"], "deterministic_replay");
            EXPECT_EQ(json["submission"]["recovered_result"], "Accepted");
            EXPECT_EQ(json["submission"]["wal_sequence"], 2);
            ASSERT_EQ(json["cancel_attempts"].size(), 3U);
            EXPECT_EQ(json["cancel_attempts"][0]["recovered_result"], "CancelNotOwner");
            EXPECT_EQ(json["cancel_attempts"][1]["recovered_result"], "Cancelled");
            EXPECT_EQ(json["cancel_attempts"][2]["recovered_result"], "CancelNotFound");
            for (std::size_t index = 0; index < 3; ++index) {
                EXPECT_EQ(json["cancel_attempts"][index]["request_id"], 2002);
                EXPECT_EQ(json["cancel_attempts"][index]["wal_sequence"], 5 + index);
            }
            EXPECT_EQ(json["trades"].size(), 2U);
            EXPECT_EQ(json["matched_quantity"], 3);
            EXPECT_EQ(json["final_state"]["status"], "not_resting");
            EXPECT_EQ(json["recovery"]["through_wal_sequence"], 7);
            EXPECT_TRUE(json["unavailable_context"]["response_delivery"].is_null());
            ASSERT_EQ(run_cli(directory, arguments), 0);
            EXPECT_EQ(read_file(directory.file("output.json")), text);
            EXPECT_EQ(read_file(wal), original);
        }
    }  // namespace
}  // namespace exchange::diagnostics
