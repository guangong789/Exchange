#include "diagnostics/partial_fill_evidence.hpp"

#include "execution/trading_bootstrap.hpp"
#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

#include <nlohmann/json.hpp>

namespace exchange::diagnostics {
    namespace {
        constexpr InstrumentContext instrument{20, 10, 1, 1, 1};
        constexpr AccountId buyer_account = 1;
        constexpr AccountId seller_account = 2;
        constexpr RequestId target_request_id = 1042;
        constexpr Price target_limit_price = 100;
        constexpr Quantity target_quantity = 5;

        void require(bool condition, const char* message) {
            if (!condition) {
                throw std::logic_error(message);
            }
        }

        class TemporaryWalDirectory {
        public:
            TemporaryWalDirectory() {
                std::string pattern =
                    (std::filesystem::temp_directory_path()
                     / "exchange-diagnostic-evidence-XXXXXX").string();
                char* created = ::mkdtemp(pattern.data());
                if (created == nullptr) {
                    throw std::system_error(
                        errno, std::generic_category(),
                        "create diagnostic WAL directory");
                }
                path_ = created;
            }

            TemporaryWalDirectory(const TemporaryWalDirectory&) = delete;
            TemporaryWalDirectory& operator=(
                const TemporaryWalDirectory&) = delete;

            ~TemporaryWalDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] std::string wal_path() const {
                return path_ + "/execution.wal";
            }

        private:
            std::string path_;
        };

        [[nodiscard]] std::vector<std::uint8_t> read_wal(
            const std::string& path) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("cannot open diagnostic WAL");
            }
            const std::streamsize size = input.tellg();
            if (size < 0) {
                throw std::runtime_error("cannot size diagnostic WAL");
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            input.seekg(0);
            if (!input || (size > 0 && !input.read(
                    reinterpret_cast<char*>(bytes.data()), size))) {
                throw std::runtime_error("cannot read diagnostic WAL");
            }
            return bytes;
        }

        [[nodiscard]] TradingRequest submit(
            RequestId request_id,
            AccountId account_id,
            Side side,
            Price price,
            Quantity quantity) {
            return TradingRequest{
                request_id, account_id,
                SubmitTradingRequest{side, price, quantity}};
        }

        [[nodiscard]] Order accepted_order(const TradingResponse& response) {
            for (const Event& event : response.events) {
                if (const auto* accepted = std::get_if<OrderAccepted>(
                        &event.payload)) {
                    return accepted->order;
                }
            }
            throw std::logic_error("accepted order event is missing");
        }

        [[nodiscard]] const char* side_name(Side side) {
            switch (side) {
                case Side::Buy: return "BUY";
                case Side::Sell: return "SELL";
            }
            throw std::logic_error("unknown order side");
        }

        [[nodiscard]] nlohmann::json event_json(const Event& event) {
            return std::visit([](const auto& payload) -> nlohmann::json {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, OrderAccepted>
                              || std::is_same_v<Payload, OrderCancelled>) {
                    return {
                        {"type", std::is_same_v<Payload, OrderAccepted>
                            ? "ORDER_ACCEPTED" : "ORDER_CANCELLED"},
                        {"order_id", payload.order.id},
                        {"side", side_name(payload.order.side)},
                        {"price", payload.order.price},
                        {"quantity", payload.order.quantity},
                        {"order_logical_timestamp", payload.order.timestamp},
                    };
                } else if constexpr (std::is_same_v<Payload, TradeCreated>) {
                    return {
                        {"type", "TRADE_CREATED"},
                        {"buy_order_id", payload.trade.buy_order_id},
                        {"sell_order_id", payload.trade.sell_order_id},
                        {"price", payload.trade.price},
                        {"quantity", payload.trade.quantity},
                        {"logical_timestamp", payload.trade.timestamp},
                    };
                } else if constexpr (std::is_same_v<Payload, OrderFilled>) {
                    return {
                        {"type", "ORDER_FILLED"},
                        {"order_id", payload.order_id},
                        {"side", side_name(payload.side)},
                        {"filled_quantity", payload.filled_quantity},
                    };
                } else {
                    return {
                        {"type", "ORDER_PARTIALLY_FILLED"},
                        {"order_id", payload.order_id},
                        {"side", side_name(payload.side)},
                        {"filled_quantity", payload.filled_quantity},
                        {"remaining_quantity", payload.remaining_quantity},
                    };
                }
            }, event.payload);
        }
    }  // namespace

    PartialFillEvidence make_partial_fill_evidence() {
        const TradingBootstrapConfig bootstrap{{
            BootstrapAccount{
                buyer_account,
                {{instrument.quote_asset, {500, 0}}}},
            BootstrapAccount{
                seller_account,
                {{instrument.base_asset, {6, 0}}}},
        }};
        TemporaryWalDirectory directory;
        const std::string wal_path = directory.wal_path();
        std::unique_ptr<TradingRuntime> runtime =
            TradingRuntime::create_durable(instrument, wal_path, bootstrap);

        const TradingResponse executable_ask = runtime->executor().execute(
            submit(1001, seller_account, Side::Sell, 100, 2));
        const TradingResponse non_executable_ask = runtime->executor().execute(
            submit(1002, seller_account, Side::Sell, 101, 4));
        require(executable_ask.result == TradingResult::Accepted
                    && executable_ask.assigned_order_id.has_value()
                    && non_executable_ask.result == TradingResult::Accepted
                    && non_executable_ask.assigned_order_id.has_value()
                    && runtime->order_book().order_count() == 2,
                "diagnostic maker orders were not accepted");

        PartialFillEvidence evidence;
        evidence.request_id = target_request_id;
        evidence.account_id = buyer_account;
        for (OrderId id : {*executable_ask.assigned_order_id,
                           *non_executable_ask.assigned_order_id}) {
            const std::optional<Order> order =
                runtime->order_book().find_order(id);
            require(order.has_value() && order->side == Side::Sell,
                    "diagnostic opposing order is missing");
            const bool executable = order->price <= target_limit_price;
            evidence.opposing_orders_before.push_back(
                OpposingOrderEvidence{*order, executable});
            if (executable) {
                require(evidence.total_executable_quantity_before
                            <= std::numeric_limits<Quantity>::max()
                                - order->quantity,
                        "diagnostic opposing quantity overflow");
                evidence.total_executable_quantity_before += order->quantity;
            }
        }

        const TradingResponse response = runtime->executor().execute(
            submit(target_request_id, buyer_account, Side::Buy,
                   target_limit_price, target_quantity));
        require(response.result == TradingResult::Accepted
                    && response.request_id == target_request_id
                    && response.assigned_order_id.has_value(),
                "diagnostic target order was not accepted");
        evidence.submitted_order = accepted_order(response);
        require(evidence.submitted_order.id == *response.assigned_order_id,
                "accepted event and response have different order IDs");
        evidence.execution_result = response.result;
        evidence.execution_events = response.events;

        for (const LedgerEntry& entry : runtime->ledger().entries()) {
            const auto* trade = std::get_if<TradeLedgerMetadata>(
                &entry.transaction.metadata);
            if (trade != nullptr
                && (trade->trade.buy_order_id == evidence.submitted_order.id
                    || trade->trade.sell_order_id
                        == evidence.submitted_order.id)) {
                evidence.trades.push_back(
                    LedgerTradeEvidence{entry.sequence, trade->trade});
                require(evidence.matched_quantity
                            <= std::numeric_limits<Quantity>::max()
                                - trade->trade.quantity,
                        "diagnostic matched quantity overflow");
                evidence.matched_quantity += trade->trade.quantity;
            }
        }
        evidence.resting_order_after = runtime->order_book().find_order(
            evidence.submitted_order.id);
        evidence.reservation_after = runtime->reservations().find(
            evidence.submitted_order.id);
        evidence.best_ask_after = runtime->order_book().best_ask();

        const std::vector<std::uint8_t> wal_bytes = read_wal(wal_path);
        const WalScanResult scan = scan_execution_wal(
            wal_bytes, instrument,
            calculate_bootstrap_fingerprint(bootstrap));
        require(scan.status == WalScanStatus::CleanEof
                    && scan.records.size() == 3,
                "diagnostic WAL did not contain the complete scenario");
        const WalRecord& target_record = scan.records.back();
        const auto* target_command = std::get_if<SubmitExecutionCommand>(
            &target_record.command);
        require(target_command != nullptr
                    && target_command->request_id == evidence.request_id
                    && target_command->account_id == evidence.account_id
                    && target_command->order.id == evidence.submitted_order.id
                    && target_command->order.timestamp
                        == evidence.submitted_order.timestamp,
                "diagnostic WAL target command does not match execution");
        evidence.wal_sequence = target_record.sequence;

        const std::optional<Order> remaining_ask =
            runtime->order_book().find_order(
                *non_executable_ask.assigned_order_id);
        require(!runtime->order_book().find_order(
                    *executable_ask.assigned_order_id).has_value()
                    && remaining_ask.has_value()
                    && remaining_ask->quantity == 4,
                "diagnostic opposing book changed unexpectedly");
        validate_partial_fill_evidence(evidence);
        return evidence;
    }

    void validate_partial_fill_evidence(
        const PartialFillEvidence& evidence) {
        const Order& target = evidence.submitted_order;
        require(evidence.request_id != 0 && evidence.account_id != 0
                    && evidence.wal_sequence != 0 && target.id != 0
                    && target.side == Side::Buy && target.price > 0
                    && target.quantity > 0 && target.timestamp > 0
                    && evidence.execution_result == TradingResult::Accepted,
                "diagnostic target identity or result is invalid");

        Quantity executable_quantity = 0;
        bool saw_executable = false;
        bool saw_non_executable = false;
        for (const OpposingOrderEvidence& opposing :
             evidence.opposing_orders_before) {
            require(opposing.order.id != 0
                        && opposing.order.side == Side::Sell
                        && opposing.order.quantity > 0
                        && opposing.executable
                            == (opposing.order.price <= target.price),
                    "diagnostic opposing order is invalid");
            if (opposing.executable) {
                saw_executable = true;
                require(executable_quantity
                            <= std::numeric_limits<Quantity>::max()
                                - opposing.order.quantity,
                        "diagnostic executable quantity overflow");
                executable_quantity += opposing.order.quantity;
            } else {
                saw_non_executable = true;
            }
        }
        require(saw_executable && saw_non_executable
                    && executable_quantity
                        == evidence.total_executable_quantity_before,
                "diagnostic opposing liquidity is incomplete");

        Quantity ledger_quantity = 0;
        std::vector<Trade> ledger_trades;
        for (const LedgerTradeEvidence& entry : evidence.trades) {
            require(entry.ledger_sequence != 0
                        && entry.trade.buy_order_id == target.id
                        && entry.trade.quantity > 0
                        && entry.trade.price <= target.price
                        && entry.trade.timestamp == target.timestamp
                        && ledger_quantity <= target.quantity
                            - entry.trade.quantity,
                    "diagnostic Ledger trade is invalid");
            ledger_quantity += entry.trade.quantity;
            ledger_trades.push_back(entry.trade);
        }
        require(!ledger_trades.empty()
                    && ledger_quantity == evidence.matched_quantity
                    && ledger_quantity
                        == evidence.total_executable_quantity_before,
                "diagnostic matched quantity disagrees with evidence");

        bool saw_accepted = false;
        bool saw_partial = false;
        std::vector<Trade> event_trades;
        for (const Event& event : evidence.execution_events) {
            if (const auto* accepted = std::get_if<OrderAccepted>(
                    &event.payload)) {
                require(!saw_accepted
                            && accepted->order.id == target.id
                            && accepted->order.side == target.side
                            && accepted->order.price == target.price
                            && accepted->order.quantity == target.quantity
                            && accepted->order.timestamp == target.timestamp,
                        "diagnostic accepted event is inconsistent");
                saw_accepted = true;
            } else if (const auto* trade = std::get_if<TradeCreated>(
                           &event.payload)) {
                event_trades.push_back(trade->trade);
            } else if (const auto* partial =
                           std::get_if<OrderPartiallyFilled>(
                               &event.payload)) {
                if (partial->order_id == target.id) {
                    require(!saw_partial && partial->side == Side::Buy
                                && partial->filled_quantity
                                    == evidence.matched_quantity
                                && evidence.resting_order_after.has_value()
                                && partial->remaining_quantity
                                    == evidence.resting_order_after->quantity,
                            "diagnostic partial-fill event is inconsistent");
                    saw_partial = true;
                }
            }
        }
        require(saw_accepted && saw_partial
                    && event_trades == ledger_trades,
                "diagnostic execution events disagree with Ledger");

        require(evidence.resting_order_after.has_value()
                    && evidence.reservation_after.has_value(),
                "diagnostic target is not resting with a reservation");
        const Order& resting = *evidence.resting_order_after;
        require(resting.id == target.id && resting.side == target.side
                    && resting.price == target.price
                    && resting.timestamp == target.timestamp
                    && resting.quantity > 0
                    && evidence.matched_quantity < target.quantity
                    && resting.quantity
                        == target.quantity - evidence.matched_quantity,
                "diagnostic remaining quantity is inconsistent");
        const OrderReservation& reservation = *evidence.reservation_after;
        require(reservation.account_id == evidence.account_id
                    && reservation.asset_id == instrument.quote_asset
                    && reservation.remaining_amount
                        == calculate_order_reservation(
                            instrument, Side::Buy, resting.price,
                            resting.quantity).amount
                    && evidence.best_ask_after.has_value()
                    && *evidence.best_ask_after > resting.price,
                "diagnostic resting state is inconsistent");
    }

    std::string partial_fill_evidence_json(
        const PartialFillEvidence& evidence) {
        validate_partial_fill_evidence(evidence);
        const Order& order = evidence.submitted_order;
        const Order& resting = *evidence.resting_order_after;
        const OrderReservation& reservation = *evidence.reservation_after;

        nlohmann::json opposing_orders = nlohmann::json::array();
        for (const OpposingOrderEvidence& opposing :
             evidence.opposing_orders_before) {
            opposing_orders.push_back({
                {"order_id", opposing.order.id},
                {"side", side_name(opposing.order.side)},
                {"price", opposing.order.price},
                {"quantity", opposing.order.quantity},
                {"executable", opposing.executable},
            });
        }
        nlohmann::json execution_events = nlohmann::json::array();
        for (const Event& event : evidence.execution_events) {
            execution_events.push_back(event_json(event));
        }
        nlohmann::json trades = nlohmann::json::array();
        for (const LedgerTradeEvidence& entry : evidence.trades) {
            trades.push_back({
                {"buy_order_id", entry.trade.buy_order_id},
                {"sell_order_id", entry.trade.sell_order_id},
                {"price", entry.trade.price},
                {"quantity", entry.trade.quantity},
                {"logical_timestamp", entry.trade.timestamp},
                {"ledger_sequence", entry.ledger_sequence},
            });
        }
        const nlohmann::json document = {
            {"case_type", "partial_fill"},
            {"target_order", {
                {"order_id", order.id},
                {"side", side_name(order.side)},
                {"limit_price", order.price},
                {"submitted_quantity", order.quantity},
            }},
            {"pre_execution_book", {
                {"opposing_orders", opposing_orders},
                {"total_executable_quantity",
                 evidence.total_executable_quantity_before},
            }},
            {"execution", {
                {"result", "ACCEPTED"},
                {"events", execution_events},
            }},
            {"trades", trades},
            {"matched_quantity", evidence.matched_quantity},
            {"post_execution", {
                {"resting", true},
                {"order_id", resting.id},
                {"side", side_name(resting.side)},
                {"limit_price", resting.price},
                {"remaining_quantity", resting.quantity},
                {"best_ask", *evidence.best_ask_after},
                {"reservation", {
                    {"account_id", reservation.account_id},
                    {"asset_id", reservation.asset_id},
                    {"remaining_amount", reservation.remaining_amount},
                }},
            }},
            {"provenance", {
                {"request_id", evidence.request_id},
                {"account_id", evidence.account_id},
                {"order_id", order.id},
                {"order_logical_timestamp", order.timestamp},
                {"wal_sequence", evidence.wal_sequence},
            }},
        };
        return document.dump(2);
    }
}  // namespace exchange::diagnostics
