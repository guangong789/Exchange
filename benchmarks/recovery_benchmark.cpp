#include "durability/execution_wal.hpp"
#include "execution/trading_runtime.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

namespace exchange {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr InstrumentContext kInstrument{20, 10, 1, 1, 1};
        constexpr Amount kInitialBalance = 1'000'000'000'000'000;
        constexpr Price kPrice = 100;
        constexpr Quantity kQuantity = 1;
        constexpr std::array<std::size_t, 3> kDefaultRecordCounts{
            100, 1'000, 10'000};

        TradingBootstrapConfig benchmark_bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{kInstrument.base_asset, {kInitialBalance, 0}},
                     {kInstrument.quote_asset, {kInitialBalance, 0}}}},
                BootstrapAccount{
                    2,
                    {{kInstrument.base_asset, {kInitialBalance, 0}},
                     {kInstrument.quote_asset, {kInitialBalance, 0}}}},
            }};
        }

        class TemporaryWal {
        public:
            TemporaryWal() {
                std::string pattern = "/tmp/exchange-recovery-benchmark-XXXXXX";
                const int fd = ::mkstemp(pattern.data());
                if (fd == -1) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "create recovery benchmark WAL");
                }
                path_ = std::move(pattern);
                if (::close(fd) == -1) {
                    const int close_error = errno;
                    static_cast<void>(::unlink(path_.c_str()));
                    throw std::system_error(
                        close_error,
                        std::generic_category(),
                        "close recovery benchmark WAL");
                }
            }

            ~TemporaryWal() {
                static_cast<void>(::unlink(path_.c_str()));
            }

            TemporaryWal(const TemporaryWal&) = delete;
            TemporaryWal& operator=(const TemporaryWal&) = delete;

            [[nodiscard]] const std::string& path() const noexcept {
                return path_;
            }

        private:
            std::string path_;
        };

        [[nodiscard]] TradingRequest submit(
            RequestId request_id,
            AccountId account_id,
            Side side,
            Price price = kPrice) {
            return TradingRequest{
                request_id,
                account_id,
                SubmitTradingRequest{side, price, kQuantity}};
        }

        void generate_wal(const std::string& path, std::size_t record_count) {
            std::unique_ptr<TradingRuntime> runtime =
                TradingRuntime::create_durable(
                    kInstrument,
                    path,
                    benchmark_bootstrap());
            for (std::size_t index = 0; index < record_count; index += 2) {
                const TradingResponse sell = runtime->executor().execute(
                    submit(index + 1, 1, Side::Sell));
                const TradingResponse buy = runtime->executor().execute(
                    submit(index + 2, 2, Side::Buy));
                if (sell.result != TradingResult::Accepted ||
                    buy.result != TradingResult::Accepted) {
                    throw std::runtime_error(
                        "deterministic recovery workload was rejected");
                }
            }
        }

        [[nodiscard]] std::size_t current_rss_kib() {
            std::ifstream statm{"/proc/self/statm"};
            std::size_t total_pages = 0;
            std::size_t resident_pages = 0;
            if (!(statm >> total_pages >> resident_pages)) {
                return 0;
            }
            static_cast<void>(total_pages);
            const long page_size = ::sysconf(_SC_PAGESIZE);
            if (page_size <= 0) {
                return 0;
            }
            return resident_pages * static_cast<std::size_t>(page_size) / 1024;
        }

        [[nodiscard]] WalScanResult scan_wal(const std::string& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("open recovery benchmark WAL");
            }
            const std::vector<std::uint8_t> bytes{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}};
            return scan_execution_wal(
                bytes,
                kInstrument,
                calculate_bootstrap_fingerprint(benchmark_bootstrap()));
        }

        void validate_recovered_state(
            const TradingRuntime& runtime,
            std::size_t record_count) {
            const Amount trade_count = static_cast<Amount>(record_count / 2);
            if (runtime.order_book().order_count() != 0 ||
                !runtime.reservations().entries().empty() ||
                runtime.ledger().entries().size() !=
                    record_count + record_count / 2 ||
                runtime.accounts().find_balance(1, kInstrument.base_asset) !=
                    std::optional<Balance>{
                        Balance{kInitialBalance - trade_count, 0}} ||
                runtime.accounts().find_balance(1, kInstrument.quote_asset) !=
                    std::optional<Balance>{
                        Balance{kInitialBalance + kPrice * trade_count, 0}} ||
                runtime.accounts().find_balance(2, kInstrument.base_asset) !=
                    std::optional<Balance>{
                        Balance{kInitialBalance + trade_count, 0}} ||
                runtime.accounts().find_balance(2, kInstrument.quote_asset) !=
                    std::optional<Balance>{
                        Balance{kInitialBalance - kPrice * trade_count, 0}}) {
                throw std::runtime_error("recovered state validation failed");
            }
        }

        void run_case(std::size_t record_count) {
            if (record_count == 0 || record_count % 2 != 0) {
                throw std::invalid_argument(
                    "recovery record count must be a positive even number");
            }

            TemporaryWal wal;
            generate_wal(wal.path(), record_count);
            const std::uintmax_t wal_bytes = std::filesystem::file_size(wal.path());
            const std::size_t rss_before_kib = current_rss_kib();

            const auto start = Clock::now();
            std::unique_ptr<TradingRuntime> recovered =
                TradingRuntime::create_durable(
                    kInstrument,
                    wal.path(),
                    benchmark_bootstrap());
            const auto end = Clock::now();

            const std::size_t rss_after_kib = current_rss_kib();
            validate_recovered_state(*recovered, record_count);
            const WalScanResult scan = scan_wal(wal.path());
            if (scan.status != WalScanStatus::CleanEof ||
                scan.records.size() != record_count ||
                scan.records.back().sequence != record_count) {
                throw std::runtime_error("recovered WAL prefix validation failed");
            }

            const WalSequence recovered_next_wal_sequence =
                scan.records.back().sequence + 1;
            const TradingResponse identity_probe = recovered->executor().execute(
                submit(record_count + 1, 2, Side::Buy, 50));
            if (identity_probe.result != TradingResult::Accepted ||
                identity_probe.assigned_order_id != record_count + 1 ||
                identity_probe.events.empty()) {
                throw std::runtime_error(
                    "recovered execution identity validation failed");
            }
            const auto* accepted = std::get_if<OrderAccepted>(
                &identity_probe.events.front().payload);
            if (accepted == nullptr ||
                accepted->order.timestamp !=
                    static_cast<Timestamp>(record_count + 1)) {
                throw std::runtime_error(
                    "recovered logical timestamp validation failed");
            }

            const double recovery_ms =
                std::chrono::duration<double, std::milli>(end - start).count();
            const std::size_t rss_delta_kib = rss_after_kib > rss_before_kib
                ? rss_after_kib - rss_before_kib
                : 0;
            std::cout
                << "records=" << record_count
                << " wal_bytes=" << wal_bytes
                << " recovery_ms=" << recovery_ms
                << " rss_before_kib=" << rss_before_kib
                << " rss_after_kib=" << rss_after_kib
                << " approximate_rss_delta_kib=" << rss_delta_kib
                << " recovered_next_order_id="
                << *identity_probe.assigned_order_id
                << " recovered_next_timestamp="
                << accepted->order.timestamp
                << " recovered_next_wal_sequence="
                << recovered_next_wal_sequence
                << " validation=PASS\n";
        }

        [[nodiscard]] std::size_t parse_record_count(std::string_view text) {
            std::uint64_t value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc{} ||
                end != text.data() + text.size() || value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("invalid WAL record count");
            }
            return static_cast<std::size_t>(value);
        }
    }  // namespace
}  // namespace exchange

int main(int argc, char** argv) {
    try {
        std::cout
            << "scope: production TradingRuntime::create_durable recovery\n"
            << "memory_note: RSS is a Linux current-resident approximation; "
               "the delta is not a process-isolated peak\n";
        if (argc == 1) {
            for (const std::size_t count : exchange::kDefaultRecordCounts) {
                exchange::run_case(count);
            }
            return 0;
        }
        for (int index = 1; index < argc; ++index) {
            exchange::run_case(exchange::parse_record_count(argv[index]));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exchange_recovery_benchmark failed: "
                  << error.what() << '\n';
        return 1;
    }
}
