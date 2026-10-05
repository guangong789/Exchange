#include "diagnostics/historical_order_evidence.hpp"
#include "historical_order_outcomes.hpp"

#include "execution/trading_runtime.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace exchange::diagnostics {
    namespace {
        void require(bool condition, const char* message) {
            if (!condition) {
                throw std::logic_error(message);
            }
        }

        class ReadOnlyWal {
        public:
            explicit ReadOnlyWal(const std::string& path)
                : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)) {
                if (fd_ == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "open offline WAL");
                }
            }
            ~ReadOnlyWal() { ::close(fd_); }
            ReadOnlyWal(const ReadOnlyWal&) = delete;
            ReadOnlyWal& operator=(const ReadOnlyWal&) = delete;

            WalBytes read() const {
                // Cooperates with the production writer's exclusive flock.
                if (::flock(fd_, LOCK_SH | LOCK_NB) == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "lock offline WAL");
                }
                struct stat status {};
                if (::fstat(fd_, &status) == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "stat offline WAL");
                }
                if (!S_ISREG(status.st_mode) || status.st_size < 0
                    || static_cast<std::uintmax_t>(status.st_size)
                        > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("offline WAL must be a regular file");
                }
                WalBytes bytes(static_cast<std::size_t>(status.st_size));
                std::size_t offset = 0;
                while (offset < bytes.size()) {
                    const ssize_t count = ::pread(
                        fd_, bytes.data() + offset, bytes.size() - offset,
                        static_cast<off_t>(offset));
                    if (count > 0) {
                        offset += static_cast<std::size_t>(count);
                    } else if (count == -1 && errno == EINTR) {
                        continue;
                    } else {
                        throw std::runtime_error("cannot read complete offline WAL");
                    }
                }
                return bytes;
            }

        private:
            int fd_;
        };

        class RecoveryCopy {
        public:
            RecoveryCopy() {
                std::string pattern = (std::filesystem::temp_directory_path()
                    / "exchange-historical-evidence-XXXXXX").string();
                const char* created = ::mkdtemp(pattern.data());
                if (created == nullptr) {
                    throw std::system_error(
                        errno, std::generic_category(), "create recovery copy directory");
                }
                directory_ = created;
            }
            ~RecoveryCopy() {
                std::error_code ignored;
                std::filesystem::remove_all(directory_, ignored);
            }
            RecoveryCopy(const RecoveryCopy&) = delete;
            RecoveryCopy& operator=(const RecoveryCopy&) = delete;

            std::string wal_path() const { return directory_ + "/execution.wal"; }

            void write(const WalBytes& bytes) const {
                std::ofstream output(wal_path(), std::ios::binary);
                if (bytes.size() > static_cast<std::size_t>(
                        std::numeric_limits<std::streamsize>::max())) {
                    throw std::length_error("offline WAL is too large to copy");
                }
                output.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output) {
                    throw std::runtime_error("cannot write recovery WAL copy");
                }
            }

        private:
            std::string directory_;
        };

        template <typename Integer>
        Integer integer(const nlohmann::json& value) {
            // Reject floats, booleans, and narrowing rather than coercing them.
            if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (number <= static_cast<std::uint64_t>(
                        std::numeric_limits<Integer>::max())) {
                    return static_cast<Integer>(number);
                }
            } else if (value.is_number_integer()) {
                const auto number = value.get<std::int64_t>();
                if (number >= 0 && static_cast<std::uint64_t>(number)
                    <= static_cast<std::uint64_t>(
                        std::numeric_limits<Integer>::max())) {
                    return static_cast<Integer>(number);
                }
            }
            throw std::invalid_argument("config values must be representable nonnegative integers");
        }

        const char* side_name(Side side) {
            switch (side) {
                case Side::Buy: return "BUY";
                case Side::Sell: return "SELL";
            }
            throw std::logic_error("unknown historical order side");
        }

        const char* state_name(HistoricalOrderState state) {
            switch (state) {
                case HistoricalOrderState::Resting: return "resting";
                case HistoricalOrderState::FullyFilled: return "fully_filled";
                case HistoricalOrderState::NotResting: return "not_resting";
            }
            throw std::logic_error("unknown historical order state");
        }

        const char* trading_result_name(TradingResult result) {
            switch (result) {
                case TradingResult::Accepted: return "Accepted";
                case TradingResult::Cancelled: return "Cancelled";
                case TradingResult::AccountNotFound: return "AccountNotFound";
                case TradingResult::InsufficientFunds: return "InsufficientFunds";
                case TradingResult::DuplicateOrder: return "DuplicateOrder";
                case TradingResult::InvalidOrder: return "InvalidOrder";
                case TradingResult::CounterpartyNotAccountBacked:
                    return "CounterpartyNotAccountBacked";
                case TradingResult::CancelNotFound: return "CancelNotFound";
                case TradingResult::CancelNotOwner: return "CancelNotOwner";
                case TradingResult::InvalidRequest: return "InvalidRequest";
            }
            throw std::logic_error("unknown recovered trading result");
        }

        bool result_matches_command(TradingResult result, bool is_submit) {
            switch (result) {
                case TradingResult::Accepted:
                case TradingResult::InsufficientFunds:
                case TradingResult::DuplicateOrder:
                case TradingResult::InvalidOrder:
                case TradingResult::CounterpartyNotAccountBacked:
                    return is_submit;
                case TradingResult::Cancelled:
                case TradingResult::CancelNotFound:
                case TradingResult::CancelNotOwner:
                    return !is_submit;
                case TradingResult::AccountNotFound:
                    return true;
                case TradingResult::InvalidRequest:
                    return false;
            }
            return false;
        }
    }  // namespace

    std::optional<HistoricalOrderEvidence> detail::associate_recovered_order_outcomes(
        OrderId order_id,
        std::span<const WalRecord> records,
        std::span<const RecoveredTradingOutcome> outcomes) {
        HistoricalOrderEvidence evidence;
        bool found = false;
        std::size_t outcome_index = 0;
        for (const auto& record : records) {
            const auto* submit = std::get_if<SubmitExecutionCommand>(&record.command);
            const auto* cancel = std::get_if<CancelExecutionCommand>(&record.command);
            if (submit == nullptr && cancel == nullptr) {
                continue;
            }
            require(outcome_index < outcomes.size()
                        && outcomes[outcome_index].wal_sequence == record.sequence,
                    "trading WAL record has no unique ordered recovery outcome");
            const auto& outcome = outcomes[outcome_index++];
            require(result_matches_command(outcome.result, submit != nullptr),
                    "recovered trading result contradicts command type");
            if (submit != nullptr && submit->order.id == order_id) {
                require(!found, "multiple durable submissions have the same OrderId");
                evidence.submission = *submit;
                evidence.submission_wal_sequence = record.sequence;
                evidence.submission_recovered_result = outcome.result;
                found = true;
            } else if (cancel != nullptr && cancel->order_id == order_id) {
                evidence.cancel_attempts.push_back({*cancel, record.sequence, outcome.result});
            }
        }
        require(outcome_index == outcomes.size(),
                "recovery outcomes contain duplicate or unmatched trading records");
        if (!found) {
            return std::nullopt;
        }
        return evidence;
    }

    HistoricalEvidenceConfig load_historical_evidence_config(
        const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("cannot open historical evidence config");
        }
        const auto document = nlohmann::json::parse(input);
        const auto& instrument = document.at("instrument");
        HistoricalEvidenceConfig config{
            InstrumentContext{
                integer<AssetId>(instrument.at("base_asset")),
                integer<AssetId>(instrument.at("quote_asset")),
                integer<Amount>(instrument.at("base_atomic_units_per_quantity_unit")),
                integer<Amount>(instrument.at("quote_atomic_units_per_price_quantity_numerator")),
                integer<Amount>(instrument.at("quote_atomic_units_per_price_quantity_denominator"))},
            {}};
        const auto& accounts = document.at("bootstrap").at("accounts");
        if (!accounts.is_array()) {
            throw std::invalid_argument("bootstrap.accounts must be an array");
        }
        for (const auto& account : accounts) {
            BootstrapAccount parsed{integer<AccountId>(account.at("account_id")), {}};
            const auto& balances = account.at("balances");
            if (!balances.is_array()) {
                throw std::invalid_argument("bootstrap balances must be an array");
            }
            for (const auto& balance : balances) {
                const auto& buckets = balance.at("balance");
                parsed.balances.push_back(BootstrapBalance{
                    integer<AssetId>(balance.at("asset_id")),
                    Balance{integer<Amount>(buckets.at("available")),
                            integer<Amount>(buckets.at("reserved"))}});
            }
            config.bootstrap.accounts.push_back(std::move(parsed));
        }
        validate_instrument_context(config.instrument);
        static_cast<void>(calculate_bootstrap_fingerprint(config.bootstrap));
        return config;
    }

    std::optional<HistoricalOrderEvidence> reconstruct_historical_order_evidence(
        const HistoricalEvidenceConfig& config,
        const std::string& offline_wal_path,
        OrderId order_id) {
        if (order_id == 0) {
            throw std::invalid_argument("historical OrderId must be positive");
        }
        validate_instrument_context(config.instrument);
        const auto fingerprint = calculate_bootstrap_fingerprint(config.bootstrap);
        const WalBytes bytes = ReadOnlyWal{offline_wal_path}.read();
        const WalScanResult scan = scan_execution_wal(
            bytes, config.instrument, fingerprint);
        if (scan.status == WalScanStatus::Error) {
            throw std::runtime_error("offline WAL validation failed");
        }
        RecoveryCopy copy;
        copy.write(bytes);
        // Uses full normal recovery, including torn-tail repair on the COPY.
        // No prefix replay or diagnostics-specific command application exists.
        std::vector<RecoveredTradingOutcome> outcomes;
        const auto runtime = TradingRuntime::create_durable(
            config.instrument, copy.wal_path(), config.bootstrap, &outcomes);

        auto associated = detail::associate_recovered_order_outcomes(
            order_id, scan.records, outcomes);
        if (!associated) {
            return std::nullopt;
        }
        HistoricalOrderEvidence evidence = std::move(*associated);
        evidence.recovered_through_wal_sequence = scan.records.empty()
            ? 0 : scan.records.back().sequence;
        evidence.ignored_torn_tail = scan.status == WalScanStatus::TornTail;
        const Order& submitted = evidence.submission.order;
        for (const auto& entry : runtime->ledger().entries()) {
            const auto* metadata = std::get_if<TradeLedgerMetadata>(
                &entry.transaction.metadata);
            if (metadata == nullptr || (metadata->trade.buy_order_id != order_id
                && metadata->trade.sell_order_id != order_id)) {
                continue;
            }
            const Trade& trade = metadata->trade;
            require(trade.quantity > 0
                        && trade.quantity <= submitted.quantity - evidence.matched_quantity
                        && (submitted.side == Side::Buy
                            ? trade.buy_order_id == order_id && trade.price <= submitted.price
                            : trade.sell_order_id == order_id && trade.price >= submitted.price),
                    "historical Ledger trade contradicts submission");
            evidence.trades.push_back({entry.sequence, trade});
            evidence.matched_quantity += trade.quantity;
        }
        evidence.resting_order = runtime->order_book().find_order(order_id);
        evidence.reservation = runtime->reservations().find(order_id);
        if (evidence.resting_order) {
            const auto& resting = *evidence.resting_order;
            require(resting.side == submitted.side && resting.price == submitted.price
                        && resting.timestamp == submitted.timestamp
                        && resting.quantity > 0
                        && resting.quantity == submitted.quantity - evidence.matched_quantity
                        && evidence.reservation.has_value()
                        && evidence.reservation->account_id == evidence.submission.account_id,
                    "historical resting state contradicts durable submission or Ledger");
            evidence.final_state = HistoricalOrderState::Resting;
        } else {
            require(!evidence.reservation, "non-resting order still has a reservation");
            if (evidence.matched_quantity == submitted.quantity) {
                evidence.final_state = HistoricalOrderState::FullyFilled;
            }
            // Absence with fewer trades proves only not_resting. In particular,
            // do not claim cancellation or acceptance from WAL admission alone.
        }
        return evidence;
    }

    std::string historical_order_evidence_json(const HistoricalOrderEvidence& evidence) {
        const auto& command = evidence.submission;
        const auto& order = command.order;
        nlohmann::json trades = nlohmann::json::array();
        for (const auto& entry : evidence.trades) {
            trades.push_back({
                {"buy_order_id", entry.trade.buy_order_id},
                {"sell_order_id", entry.trade.sell_order_id},
                {"price", entry.trade.price}, {"quantity", entry.trade.quantity},
                {"logical_timestamp", entry.trade.timestamp},
                {"ledger_sequence", entry.ledger_sequence}});
        }
        nlohmann::json cancel_attempts = nlohmann::json::array();
        for (const auto& attempt : evidence.cancel_attempts) {
            cancel_attempts.push_back({
                {"request_id", attempt.command.request_id},
                {"account_id", attempt.command.account_id},
                {"order_id", attempt.command.order_id},
                {"wal_sequence", attempt.wal_sequence},
                {"recovered_result", trading_result_name(attempt.recovered_result)}});
        }
        nlohmann::json remaining = nullptr;
        if (evidence.resting_order) {
            remaining = evidence.resting_order->quantity;
        } else if (evidence.final_state == HistoricalOrderState::FullyFilled) {
            remaining = 0;
        }
        nlohmann::json reservation = nullptr;
        if (evidence.reservation) {
            reservation = {
                {"account_id", evidence.reservation->account_id},
                {"asset_id", evidence.reservation->asset_id},
                {"original_amount", evidence.reservation->original_amount},
                {"remaining_amount", evidence.reservation->remaining_amount}};
        }
        const nlohmann::json document = {
            {"case_type", "historical_durable_order"}, {"order_id", order.id},
            {"outcome_basis", "deterministic_replay"},
            {"submission", {
                {"request_id", command.request_id}, {"account_id", command.account_id},
                {"side", side_name(order.side)}, {"price", order.price},
                {"quantity", order.quantity}, {"logical_timestamp", order.timestamp},
                {"wal_sequence", evidence.submission_wal_sequence},
                {"recovered_result", trading_result_name(evidence.submission_recovered_result)}}},
            {"cancel_attempts", cancel_attempts},
            {"trades", trades}, {"matched_quantity", evidence.matched_quantity},
            {"final_state", {
                {"status", state_name(evidence.final_state)},
                {"remaining_quantity", remaining},
                {"side", evidence.resting_order ? nlohmann::json(side_name(order.side)) : nlohmann::json(nullptr)},
                {"price", evidence.resting_order ? nlohmann::json(order.price) : nlohmann::json(nullptr)},
                {"reservation", reservation}}},
            {"recovery", {
                {"through_wal_sequence", evidence.recovered_through_wal_sequence},
                {"ignored_torn_tail", evidence.ignored_torn_tail}}},
            {"evidence_scope", {"durable_submission", "recovered_ledger",
                                "recovered_final_state", "recovered_trading_outcomes"}},
            // These facts are not captured by this final-state reconstruction.
            {"unavailable_context", {
                {"pre_execution_book", nullptr}, {"execution_events", nullptr},
                {"response_delivery", nullptr}, {"matching_stop_reason", nullptr}}}};
        return document.dump(2);
    }
}  // namespace exchange::diagnostics
