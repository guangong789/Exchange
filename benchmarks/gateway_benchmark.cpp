#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/tcp_gateway.hpp"
#include "exchange/types.hpp"

namespace exchange {
    namespace {
        using Clock = std::chrono::steady_clock;
        using Latency = std::chrono::nanoseconds;

        constexpr std::size_t kDefaultThroughputCommandCount = 1'000'000;
        constexpr std::size_t kDefaultLatencyCommandCount = 100'000;
        constexpr std::size_t kDefaultCommandPressureCommandCount = 64;
        constexpr std::size_t kDefaultSlowReaderCommandCount = 100'000;
        constexpr std::size_t kDefaultWarmupCommandCount = 10'000;
        constexpr std::size_t kDefaultRepetitions = 10;
        constexpr std::size_t kDefaultStressRepetitions = 1;
        constexpr std::size_t kThroughputOutstandingWindow = 8;
        constexpr std::size_t kLatencyOutstandingWindow = 1;
        constexpr std::size_t kStressClientCount = 2;
        constexpr std::size_t kPressureQueueCapacity = 1;
        constexpr std::size_t kSlowReaderBatchSize = 8;
        constexpr std::size_t kMaximumStressCommandCount = 1'000'000;
        constexpr std::array<std::size_t, 4> kStandardClientCounts{
            1, 4, 16, 64};
        constexpr Price kOrderPrice = 100'000;
        constexpr Quantity kOrderQuantity = 10;
        constexpr std::chrono::seconds kNoProgressTimeout{30};
        constexpr int kPollIntervalMilliseconds = 100;
        constexpr std::size_t kReceiveBufferSize = 16 * 1024;
        constexpr std::size_t kMaximumResponseLineLength = 1024;
        constexpr std::string_view kWorkloadName = "paired-cross-v1";
        constexpr std::string_view kCommandPressureWorkloadName =
            "command-admission-overload-v1";
        constexpr std::string_view kSlowReaderWorkloadName =
            "slow-reader-output-limit-v1";

        enum class Scenario {
            Throughput,
            Latency,
            CommandPressure,
            SlowReader,
        };

        struct BenchmarkConfig {
            Scenario scenario{Scenario::Throughput};
            std::vector<std::size_t> client_counts{1};
            std::size_t command_count{kDefaultThroughputCommandCount};
            std::size_t warmup_command_count{kDefaultWarmupCommandCount};
            std::size_t repetitions{kDefaultRepetitions};
            bool clients_were_explicit{};
        };

        struct Workload {
            std::vector<std::vector<std::string>> requests_by_client;
            std::size_t command_count{};
            std::size_t expected_trade_count{};
        };

        struct PhaseResult {
            std::size_t sent_commands{};
            std::size_t completed_responses{};
            std::size_t trade_count{};
            std::size_t maximum_per_client_outstanding{};
            std::size_t maximum_total_outstanding{};
            Clock::duration elapsed{};
            std::vector<Latency> latencies;
        };

        struct LatencySummary {
            std::size_t sample_count{};
            Latency p50{};
            Latency p95{};
            Latency p99{};
            Latency maximum{};
        };

        class ScopedFd {
        public:
            explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

            ~ScopedFd() {
                reset();
            }

            ScopedFd(const ScopedFd&) = delete;
            ScopedFd& operator=(const ScopedFd&) = delete;

            ScopedFd(ScopedFd&& other) noexcept
                : fd_(std::exchange(other.fd_, -1)) {}

            ScopedFd& operator=(ScopedFd&&) = delete;

            [[nodiscard]] int get() const noexcept {
                return fd_;
            }

            void reset() noexcept {
                if (fd_ != -1) {
                    ::close(fd_);
                    fd_ = -1;
                }
            }

        private:
            int fd_{-1};
        };

        class RunningGateway {
        public:
            explicit RunningGateway(
                std::size_t command_queue_capacity =
                    kDefaultCommandQueueCapacity,
                std::size_t response_queue_capacity =
                    kDefaultResponseQueueCapacity)
                : gateway_(
                      0,
                      command_queue_capacity,
                      response_queue_capacity),
                  io_thread_([this] { run_io_loop(); }) {}

            ~RunningGateway() {
                stop();
            }

            RunningGateway(const RunningGateway&) = delete;
            RunningGateway& operator=(const RunningGateway&) = delete;
            RunningGateway(RunningGateway&&) = delete;
            RunningGateway& operator=(RunningGateway&&) = delete;

            [[nodiscard]] std::uint16_t local_port() const noexcept {
                return gateway_.local_port();
            }

            void rethrow_if_stopped() const {
                if (!io_thread_exited_.load(std::memory_order_acquire)) {
                    return;
                }
                if (io_thread_failure_) {
                    std::rethrow_exception(io_thread_failure_);
                }
                throw std::runtime_error(
                    "gateway I/O thread stopped unexpectedly");
            }

            void stop_and_check() {
                stop();
                if (io_thread_failure_) {
                    std::rethrow_exception(io_thread_failure_);
                }
            }

        private:
            void run_io_loop() noexcept {
                try {
                    gateway_.run();
                } catch (...) {
                    io_thread_failure_ = std::current_exception();
                }
                io_thread_exited_.store(true, std::memory_order_release);
            }

            void stop() noexcept {
                gateway_.request_stop();
                if (io_thread_.joinable()) {
                    io_thread_.join();
                }
            }

            TcpGateway gateway_;
            std::exception_ptr io_thread_failure_;
            std::atomic_bool io_thread_exited_{};
            std::thread io_thread_;
        };

        class ResponseParser {
        public:
            void append(std::string_view bytes) {
                buffer_.append(bytes);
                parse_available_lines();
            }

            [[nodiscard]] std::size_t completed_responses() const noexcept {
                return completed_responses_;
            }

            [[nodiscard]] std::size_t trade_count() const noexcept {
                return trade_count_;
            }

            [[nodiscard]] bool idle() const noexcept {
                return remaining_event_lines_ == 0 &&
                       read_position_ == buffer_.size();
            }

        private:
            void parse_available_lines() {
                while (true) {
                    const std::size_t newline =
                        buffer_.find('\n', read_position_);
                    if (newline == std::string::npos) {
                        if (buffer_.size() - read_position_ >
                            kMaximumResponseLineLength) {
                            throw std::runtime_error(
                                "gateway response line exceeds benchmark limit");
                        }
                        compact();
                        return;
                    }

                    const std::size_t line_length = newline - read_position_;
                    if (line_length > kMaximumResponseLineLength) {
                        throw std::runtime_error(
                            "gateway response line exceeds benchmark limit");
                    }

                    const std::string_view line{
                        buffer_.data() + read_position_, line_length};
                    parse_line(line);
                    read_position_ = newline + 1;
                }
            }

            void parse_line(std::string_view line) {
                if (remaining_event_lines_ != 0) {
                    if (!line.starts_with("EVENT ")) {
                        throw std::runtime_error(
                            "expected EVENT line in gateway response");
                    }
                    if (line.starts_with("EVENT TRADE_CREATED ")) {
                        ++trade_count_;
                    }

                    --remaining_event_lines_;
                    if (remaining_event_lines_ == 0) {
                        ++completed_responses_;
                    }
                    return;
                }

                if (line.starts_with("ERR ")) {
                    throw std::runtime_error(
                        "gateway returned protocol error: " +
                        std::string{line});
                }
                if (!line.starts_with("OK ")) {
                    throw std::runtime_error(
                        "invalid gateway response header");
                }

                const std::string_view count_text = line.substr(3);
                std::size_t event_count = 0;
                const auto [parsed_to, error] = std::from_chars(
                    count_text.data(),
                    count_text.data() + count_text.size(),
                    event_count);
                if (error != std::errc{} ||
                    parsed_to != count_text.data() + count_text.size()) {
                    throw std::runtime_error(
                        "invalid event count in gateway response");
                }

                remaining_event_lines_ = event_count;
                if (remaining_event_lines_ == 0) {
                    ++completed_responses_;
                }
            }

            void compact() {
                if (read_position_ == buffer_.size()) {
                    buffer_.clear();
                    read_position_ = 0;
                    return;
                }

                if (read_position_ >= 4096 &&
                    read_position_ >= buffer_.size() / 2) {
                    buffer_.erase(0, read_position_);
                    read_position_ = 0;
                }
            }

            std::string buffer_;
            std::size_t read_position_{};
            std::size_t remaining_event_lines_{};
            std::size_t completed_responses_{};
            std::size_t trade_count_{};
        };

        class CapturedResponseParser {
        public:
            void append(std::string_view bytes) {
                buffer_.append(bytes);
                parse_available_lines();
            }

            [[nodiscard]] bool has_response() const noexcept {
                return !completed_responses_.empty();
            }

            [[nodiscard]] std::string take_response() {
                if (completed_responses_.empty()) {
                    throw std::logic_error("no captured response available");
                }
                std::string response =
                    std::move(completed_responses_.front());
                completed_responses_.pop_front();
                return response;
            }

        private:
            void parse_available_lines() {
                while (true) {
                    const std::size_t newline =
                        buffer_.find('\n', read_position_);
                    if (newline == std::string::npos) {
                        if (buffer_.size() - read_position_ >
                            kMaximumResponseLineLength) {
                            throw std::runtime_error(
                                "gateway stress response line exceeds limit");
                        }
                        compact();
                        return;
                    }

                    const std::size_t line_length = newline - read_position_;
                    if (line_length > kMaximumResponseLineLength) {
                        throw std::runtime_error(
                            "gateway stress response line exceeds limit");
                    }

                    const std::string_view line{
                        buffer_.data() + read_position_, line_length};
                    parse_line(line);
                    read_position_ = newline + 1;
                }
            }

            void parse_line(std::string_view line) {
                if (remaining_event_lines_ == 0) {
                    if (line.starts_with("ERR ")) {
                        throw std::runtime_error(
                            "healthy client received protocol error: " +
                            std::string{line});
                    }
                    if (!line.starts_with("OK ")) {
                        throw std::runtime_error(
                            "invalid stress response header");
                    }

                    const std::string_view count_text = line.substr(3);
                    std::size_t event_count = 0;
                    const auto [parsed_to, error] = std::from_chars(
                        count_text.data(),
                        count_text.data() + count_text.size(),
                        event_count);
                    if (error != std::errc{} ||
                        parsed_to != count_text.data() + count_text.size()) {
                        throw std::runtime_error(
                            "invalid stress response event count");
                    }

                    current_response_.assign(line);
                    current_response_ += '\n';
                    remaining_event_lines_ = event_count;
                    if (remaining_event_lines_ == 0) {
                        finish_response();
                    }
                    return;
                }

                if (!line.starts_with("EVENT ")) {
                    throw std::runtime_error(
                        "expected EVENT line in stress response");
                }
                current_response_.append(line);
                current_response_ += '\n';
                --remaining_event_lines_;
                if (remaining_event_lines_ == 0) {
                    finish_response();
                }
            }

            void finish_response() {
                completed_responses_.push_back(
                    std::move(current_response_));
                current_response_.clear();
            }

            void compact() {
                if (read_position_ == buffer_.size()) {
                    buffer_.clear();
                    read_position_ = 0;
                    return;
                }
                if (read_position_ >= 4096 &&
                    read_position_ >= buffer_.size() / 2) {
                    buffer_.erase(0, read_position_);
                    read_position_ = 0;
                }
            }

            std::string buffer_;
            std::size_t read_position_{};
            std::size_t remaining_event_lines_{};
            std::string current_response_;
            std::deque<std::string> completed_responses_;
        };

        class OutstandingFifo {
        public:
            explicit OutstandingFifo(std::size_t capacity)
                : entries_(capacity) {
                if (capacity == 0) {
                    throw std::invalid_argument(
                        "outstanding FIFO capacity must be positive");
                }
            }

            void push(Clock::time_point sent_at) {
                if (size_ == entries_.size()) {
                    throw std::logic_error("outstanding FIFO is full");
                }
                entries_[(head_ + size_) % entries_.size()] = sent_at;
                ++size_;
            }

            [[nodiscard]] Clock::time_point pop() {
                if (size_ == 0) {
                    throw std::logic_error("outstanding FIFO is empty");
                }
                const Clock::time_point value = entries_[head_];
                head_ = (head_ + 1) % entries_.size();
                --size_;
                return value;
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return size_;
            }

            [[nodiscard]] bool empty() const noexcept {
                return size_ == 0;
            }

        private:
            std::vector<Clock::time_point> entries_;
            std::size_t head_{};
            std::size_t size_{};
        };

        struct ClientPhaseState {
            ClientPhaseState(
                int client_fd,
                const std::vector<std::string>& assigned_requests,
                std::size_t outstanding_window)
                : fd(client_fd),
                  requests(&assigned_requests),
                  outstanding(outstanding_window) {}

            int fd{-1};
            const std::vector<std::string>* requests{};
            std::size_t next_request{};
            std::size_t request_offset{};
            std::size_t sent_commands{};
            std::size_t completed_responses{};
            std::size_t maximum_outstanding{};
            ResponseParser parser;
            OutstandingFifo outstanding;
        };

        [[nodiscard]] std::string_view scenario_name(Scenario scenario) {
            switch (scenario) {
                case Scenario::Throughput:
                    return "throughput";
                case Scenario::Latency:
                    return "latency";
                case Scenario::CommandPressure:
                    return "command-pressure";
                case Scenario::SlowReader:
                    return "slow-reader";
            }
            throw std::logic_error("unknown benchmark scenario");
        }

        [[nodiscard]] std::size_t scenario_window(Scenario scenario) {
            switch (scenario) {
                case Scenario::Throughput:
                    return kThroughputOutstandingWindow;
                case Scenario::Latency:
                    return kLatencyOutstandingWindow;
                case Scenario::CommandPressure:
                case Scenario::SlowReader:
                    throw std::logic_error(
                        "stress scenario has no response window");
            }
            throw std::logic_error("unknown benchmark scenario");
        }

        [[nodiscard]] bool is_performance_scenario(Scenario scenario) {
            return scenario == Scenario::Throughput ||
                   scenario == Scenario::Latency;
        }

        [[nodiscard]] std::size_t parse_size(
            std::string_view option,
            std::string_view text) {
            std::uint64_t parsed = 0;
            const auto [parsed_to, error] = std::from_chars(
                text.data(), text.data() + text.size(), parsed);
            if (text.empty() || error != std::errc{} ||
                parsed_to != text.data() + text.size() ||
                parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "invalid value for " + std::string{option});
            }
            return static_cast<std::size_t>(parsed);
        }

        [[nodiscard]] bool is_supported_client_count(std::size_t value) {
            return std::find(
                       kStandardClientCounts.begin(),
                       kStandardClientCounts.end(),
                       value) != kStandardClientCounts.end();
        }

        void print_usage(std::ostream& output) {
            output
                << "Usage: exchange_gateway_benchmark [options]\n\n"
                << "Options:\n"
                << "  --scenario throughput|latency|command-pressure|slow-reader\n"
                << "  --clients 1|4|16|64|all\n"
                << "  --commands N\n"
                << "  --warmup-commands N\n"
                << "  --repetitions N\n"
                << "  --help\n\n"
                << "Defaults:\n"
                << "  scenario: throughput\n"
                << "  clients: 1\n"
                << "  throughput commands: "
                << kDefaultThroughputCommandCount << '\n'
                << "  latency commands: "
                << kDefaultLatencyCommandCount << '\n'
                << "  command-pressure burst commands: "
                << kDefaultCommandPressureCommandCount << '\n'
                << "  slow-reader maximum commands: "
                << kDefaultSlowReaderCommandCount << '\n'
                << "  warmup commands: "
                << kDefaultWarmupCommandCount << '\n'
                << "  performance repetitions: "
                << kDefaultRepetitions << '\n'
                << "  stress repetitions: "
                << kDefaultStressRepetitions << "\n\n"
                << "Use --clients all to run the standard 1/4/16/64 matrix.\n";
        }

        [[nodiscard]] BenchmarkConfig parse_arguments(
            int argc,
            char** argv) {
            BenchmarkConfig config;
            std::optional<std::size_t> command_count_override;
            std::optional<std::size_t> warmup_count_override;
            std::optional<std::size_t> repetitions_override;

            for (int index = 1; index < argc; ++index) {
                const std::string_view argument{argv[index]};
                if (argument == "--help") {
                    print_usage(std::cout);
                    std::exit(0);
                }
                if (argument != "--scenario" &&
                    argument != "--clients" &&
                    argument != "--commands" &&
                    argument != "--warmup-commands" &&
                    argument != "--repetitions") {
                    throw std::invalid_argument(
                        "unknown argument: " + std::string{argument});
                }
                if (index + 1 == argc) {
                    throw std::invalid_argument(
                        "missing value for " + std::string{argument});
                }

                const std::string_view value{argv[++index]};
                if (argument == "--scenario") {
                    if (value == "throughput") {
                        config.scenario = Scenario::Throughput;
                    } else if (value == "latency") {
                        config.scenario = Scenario::Latency;
                    } else if (value == "command-pressure") {
                        config.scenario = Scenario::CommandPressure;
                    } else if (value == "slow-reader") {
                        config.scenario = Scenario::SlowReader;
                    } else {
                        throw std::invalid_argument(
                            "--scenario must be throughput, latency, "
                            "command-pressure, or slow-reader");
                    }
                    continue;
                }
                if (argument == "--clients") {
                    config.clients_were_explicit = true;
                    if (value == "all") {
                        config.client_counts.assign(
                            kStandardClientCounts.begin(),
                            kStandardClientCounts.end());
                    } else {
                        const std::size_t client_count =
                            parse_size(argument, value);
                        if (!is_supported_client_count(client_count)) {
                            throw std::invalid_argument(
                                "--clients must be 1, 4, 16, 64, or all");
                        }
                        config.client_counts = {client_count};
                    }
                    continue;
                }

                const std::size_t parsed = parse_size(argument, value);
                if (argument == "--commands") {
                    command_count_override = parsed;
                } else if (argument == "--warmup-commands") {
                    warmup_count_override = parsed;
                } else {
                    repetitions_override = parsed;
                }
            }

            switch (config.scenario) {
                case Scenario::Throughput:
                    config.command_count = command_count_override.value_or(
                        kDefaultThroughputCommandCount);
                    break;
                case Scenario::Latency:
                    config.command_count = command_count_override.value_or(
                        kDefaultLatencyCommandCount);
                    break;
                case Scenario::CommandPressure:
                    config.command_count = command_count_override.value_or(
                        kDefaultCommandPressureCommandCount);
                    break;
                case Scenario::SlowReader:
                    config.command_count = command_count_override.value_or(
                        kDefaultSlowReaderCommandCount);
                    break;
            }
            config.warmup_command_count = warmup_count_override.value_or(
                is_performance_scenario(config.scenario)
                    ? kDefaultWarmupCommandCount
                    : 0);
            config.repetitions = repetitions_override.value_or(
                is_performance_scenario(config.scenario)
                    ? kDefaultRepetitions
                    : kDefaultStressRepetitions);

            if (config.repetitions == 0) {
                throw std::invalid_argument(
                    "--repetitions must be positive");
            }

            if (!is_performance_scenario(config.scenario)) {
                if (config.clients_were_explicit) {
                    throw std::invalid_argument(
                        "--clients is not used by stress scenarios; "
                        "they always use two clients");
                }
                if (config.warmup_command_count != 0) {
                    throw std::invalid_argument(
                        "stress scenarios require --warmup-commands 0");
                }
                config.client_counts = {kStressClientCount};
                if (config.scenario == Scenario::CommandPressure &&
                    config.command_count < 4) {
                    throw std::invalid_argument(
                        "command-pressure requires at least four commands");
                }
                if (config.scenario == Scenario::SlowReader &&
                    config.command_count < 2) {
                    throw std::invalid_argument(
                        "slow-reader requires at least two commands");
                }
                if (config.command_count > kMaximumStressCommandCount) {
                    throw std::invalid_argument(
                        "stress scenario command count exceeds benchmark limit");
                }
                return config;
            }

            if (config.command_count == 0 ||
                config.command_count % 2 != 0) {
                throw std::invalid_argument(
                    "--commands must be a positive even number");
            }
            if (config.warmup_command_count == 0 ||
                config.warmup_command_count % 2 != 0) {
                throw std::invalid_argument(
                    "--warmup-commands must be a positive even number");
            }

            const std::size_t maximum_logical_id =
                static_cast<std::size_t>(
                    std::numeric_limits<Timestamp>::max());
            if (config.warmup_command_count > maximum_logical_id ||
                config.command_count >
                    maximum_logical_id - config.warmup_command_count) {
                throw std::invalid_argument(
                    "combined workload exceeds OrderId/Timestamp range");
            }
            if (config.command_count >
                std::numeric_limits<std::size_t>::max() /
                    config.repetitions) {
                throw std::invalid_argument(
                    "latency sample count exceeds size_t range");
            }
            return config;
        }

        [[nodiscard]] std::string make_add_request(
            OrderId order_id,
            std::string_view side,
            Price price,
            Quantity quantity,
            Timestamp timestamp) {
            std::string output = "ADD ";
            output += std::to_string(order_id);
            output += ' ';
            output += side;
            output += ' ';
            output += std::to_string(price);
            output += ' ';
            output += std::to_string(quantity);
            output += ' ';
            output += std::to_string(timestamp);
            output += '\n';
            return output;
        }

        void append_order_request(
            std::string& output,
            OrderId order_id,
            std::string_view side,
            Timestamp timestamp) {
            output = make_add_request(
                order_id,
                side,
                kOrderPrice,
                kOrderQuantity,
                timestamp);
        }

        [[nodiscard]] Workload make_paired_cross_workload(
            std::size_t command_count,
            OrderId first_order_id,
            Timestamp first_timestamp,
            std::size_t client_count) {
            Workload workload;
            workload.requests_by_client.resize(client_count);
            workload.command_count = command_count;
            workload.expected_trade_count = command_count / 2;

            const std::size_t approximate_commands_per_client =
                (command_count + client_count - 1) / client_count;
            for (auto& requests : workload.requests_by_client) {
                requests.reserve(approximate_commands_per_client + 2);
            }

            for (std::size_t offset = 0;
                 offset < command_count;
                 offset += 2) {
                const std::size_t pair_index = offset / 2;
                const std::size_t client_index = pair_index % client_count;
                auto& requests = workload.requests_by_client[client_index];

                std::string sell_request;
                append_order_request(
                    sell_request,
                    first_order_id + offset,
                    "SELL",
                    first_timestamp + static_cast<Timestamp>(offset));
                requests.push_back(std::move(sell_request));

                std::string buy_request;
                append_order_request(
                    buy_request,
                    first_order_id + offset + 1,
                    "BUY",
                    first_timestamp + static_cast<Timestamp>(offset + 1));
                requests.push_back(std::move(buy_request));
            }
            return workload;
        }

        [[nodiscard]] ScopedFd connect_client(
            std::uint16_t port,
            std::optional<int> receive_buffer_size = std::nullopt) {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "client socket");
            }
            ScopedFd client{fd};

            if (receive_buffer_size.has_value() &&
                ::setsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_RCVBUF,
                    &*receive_buffer_size,
                    sizeof(*receive_buffer_size)) == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "client setsockopt SO_RCVBUF");
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            while (::connect(
                       fd,
                       reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(
                    errno, std::generic_category(), "client connect");
            }

            int flags = -1;
            do {
                flags = ::fcntl(fd, F_GETFL, 0);
            } while (flags == -1 && errno == EINTR);
            if (flags == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "client fcntl F_GETFL");
            }

            int set_result = -1;
            do {
                set_result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            } while (set_result == -1 && errno == EINTR);
            if (set_result == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "client fcntl F_SETFL");
            }

            return client;
        }

        [[nodiscard]] std::vector<ScopedFd> connect_clients(
            std::size_t client_count,
            std::uint16_t port) {
            std::vector<ScopedFd> clients;
            clients.reserve(client_count);
            for (std::size_t index = 0; index < client_count; ++index) {
                clients.push_back(connect_client(port));
            }
            return clients;
        }

        [[nodiscard]] int socket_error(int fd) noexcept {
            int error = 0;
            socklen_t error_length = sizeof(error);
            if (::getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &error,
                    &error_length) == -1) {
                return errno;
            }
            return error == 0 ? EIO : error;
        }

        [[nodiscard]] int stress_poll_timeout_ms(
            Clock::time_point deadline) {
            const Clock::time_point now = Clock::now();
            if (now >= deadline) {
                throw std::runtime_error("stress scenario timed out");
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
            return static_cast<int>(std::clamp<std::int64_t>(
                remaining.count() + 1,
                1,
                kPollIntervalMilliseconds));
        }

        [[nodiscard]] short poll_client(
            int fd,
            short requested_events,
            Clock::time_point deadline,
            RunningGateway& gateway) {
            while (true) {
                gateway.rethrow_if_stopped();
                pollfd event{fd, requested_events, 0};
                const int result = ::poll(
                    &event,
                    1,
                    stress_poll_timeout_ms(deadline));
                if (result > 0) {
                    return event.revents;
                }
                if (result == 0) {
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(
                    errno, std::generic_category(), "stress client poll");
            }
        }

        [[nodiscard]] bool send_before_deadline(
            int fd,
            std::string_view bytes,
            Clock::time_point deadline,
            RunningGateway& gateway) {
            std::size_t sent = 0;
            while (sent < bytes.size()) {
                gateway.rethrow_if_stopped();
                const ssize_t result = ::send(
                    fd,
                    bytes.data() + sent,
                    bytes.size() - sent,
                    MSG_NOSIGNAL);
                if (result > 0) {
                    sent += static_cast<std::size_t>(result);
                    continue;
                }
                if (result == -1 && errno == EINTR) {
                    continue;
                }
                if (result == -1 &&
                    (errno == EPIPE || errno == ECONNRESET ||
                     errno == ENOTCONN)) {
                    return false;
                }
                if (result == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    const short events = poll_client(
                        fd,
                        POLLOUT | POLLRDHUP,
                        deadline,
                        gateway);
                    if ((events & (POLLERR | POLLHUP | POLLRDHUP |
                                   POLLNVAL)) != 0) {
                        return false;
                    }
                    continue;
                }
                throw std::system_error(
                    result == -1 ? errno : EIO,
                    std::generic_category(),
                    "stress client send");
            }
            return true;
        }

        [[nodiscard]] std::string receive_response_before_deadline(
            int fd,
            CapturedResponseParser& parser,
            Clock::time_point deadline,
            RunningGateway& gateway) {
            std::array<char, kReceiveBufferSize> buffer{};

            while (!parser.has_response()) {
                gateway.rethrow_if_stopped();
                while (true) {
                    const ssize_t result = ::recv(
                        fd, buffer.data(), buffer.size(), 0);
                    if (result > 0) {
                        parser.append(std::string_view{
                            buffer.data(),
                            static_cast<std::size_t>(result)});
                        if (parser.has_response()) {
                            break;
                        }
                        continue;
                    }
                    if (result == 0) {
                        throw std::runtime_error(
                            "healthy client disconnected before response");
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "stress healthy client recv");
                }

                if (parser.has_response()) {
                    break;
                }
                const short events = poll_client(
                    fd,
                    POLLIN | POLLRDHUP,
                    deadline,
                    gateway);
                if ((events & (POLLERR | POLLHUP | POLLRDHUP |
                               POLLNVAL)) != 0) {
                    throw std::runtime_error(
                        "healthy client closed during stress scenario");
                }
            }

            return parser.take_response();
        }

        void expect_response_before_deadline(
            int fd,
            CapturedResponseParser& parser,
            std::string_view expected,
            Clock::time_point deadline,
            RunningGateway& gateway) {
            const std::string actual = receive_response_before_deadline(
                fd, parser, deadline, gateway);
            if (actual != expected) {
                throw std::runtime_error(
                    "healthy client received an unexpected or misrouted response");
            }
        }

        void wait_for_peer_close_and_discard(
            int fd,
            Clock::time_point deadline,
            RunningGateway& gateway) {
            std::array<char, kReceiveBufferSize> buffer{};
            while (true) {
                gateway.rethrow_if_stopped();
                const ssize_t result = ::recv(
                    fd, buffer.data(), buffer.size(), 0);
                if (result > 0) {
                    continue;
                }
                if (result == 0) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECONNRESET || errno == ENOTCONN) {
                    return;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "stress offending client recv");
                }

                const short events = poll_client(
                    fd,
                    POLLIN | POLLRDHUP,
                    deadline,
                    gateway);
                if ((events & POLLNVAL) != 0) {
                    throw std::runtime_error(
                        "stress offending client fd became invalid");
                }
                if ((events & POLLERR) != 0) {
                    const int error = socket_error(fd);
                    if (error == ECONNRESET || error == EPIPE) {
                        return;
                    }
                    throw std::system_error(
                        error,
                        std::generic_category(),
                        "stress offending client socket error");
                }
            }
        }

        [[nodiscard]] bool peer_close_signaled(int fd) {
            pollfd event{fd, POLLRDHUP, 0};
            int result = -1;
            do {
                result = ::poll(&event, 1, 0);
            } while (result == -1 && errno == EINTR);
            if (result == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "stress close detection poll");
            }
            return result > 0 &&
                   (event.revents &
                   (POLLERR | POLLHUP | POLLRDHUP | POLLNVAL)) != 0;
        }

        [[nodiscard]] std::string accepted_response(
            OrderId order_id,
            std::string_view side,
            Price price,
            Quantity quantity,
            Timestamp timestamp) {
            return "OK 1\nEVENT ORDER_ACCEPTED " +
                std::to_string(order_id) + " " + std::string{side} + " " +
                std::to_string(price) + " " + std::to_string(quantity) +
                " " + std::to_string(timestamp) + "\n";
        }

        [[nodiscard]] std::string full_match_response(
            OrderId buy_order_id,
            OrderId sell_order_id,
            Price price,
            Quantity quantity,
            Timestamp buy_timestamp) {
            return "OK 4\nEVENT ORDER_ACCEPTED " +
                std::to_string(buy_order_id) + " BUY " +
                std::to_string(price) + " " + std::to_string(quantity) +
                " " + std::to_string(buy_timestamp) +
                "\nEVENT TRADE_CREATED " +
                std::to_string(buy_order_id) + " " +
                std::to_string(sell_order_id) + " " +
                std::to_string(price) + " " + std::to_string(quantity) +
                " " + std::to_string(buy_timestamp) +
                "\nEVENT ORDER_FILLED " +
                std::to_string(sell_order_id) + " SELL " +
                std::to_string(quantity) +
                "\nEVENT ORDER_FILLED " +
                std::to_string(buy_order_id) + " BUY " +
                std::to_string(quantity) + "\n";
        }

        [[nodiscard]] Quantity cancelled_remaining_quantity(
            std::string_view response,
            OrderId expected_order_id,
            Price expected_price,
            Timestamp expected_timestamp) {
            const std::string prefix =
                "OK 1\nEVENT ORDER_CANCELLED " +
                std::to_string(expected_order_id) + " SELL " +
                std::to_string(expected_price) + " ";
            if (!response.starts_with(prefix)) {
                throw std::runtime_error(
                    "sentinel cancellation response was misrouted or malformed");
            }

            const std::string_view remaining = response.substr(prefix.size());
            const std::size_t separator = remaining.find(' ');
            if (separator == std::string_view::npos ||
                remaining.substr(separator + 1) !=
                    std::to_string(expected_timestamp) + "\n") {
                throw std::runtime_error(
                    "sentinel cancellation response has unexpected state");
            }

            Quantity quantity = 0;
            const std::string_view quantity_text =
                remaining.substr(0, separator);
            const auto [parsed_to, error] = std::from_chars(
                quantity_text.data(),
                quantity_text.data() + quantity_text.size(),
                quantity);
            if (error != std::errc{} ||
                parsed_to != quantity_text.data() + quantity_text.size()) {
                throw std::runtime_error(
                    "invalid sentinel remaining quantity");
            }
            return quantity;
        }

        [[nodiscard]] PhaseResult run_phase(
            const std::vector<ScopedFd>& clients,
            const Workload& workload,
            std::size_t outstanding_window,
            bool collect_latencies,
            RunningGateway& gateway) {
            if (clients.size() != workload.requests_by_client.size()) {
                throw std::logic_error(
                    "client/workload distribution count mismatch");
            }

            std::vector<ClientPhaseState> states;
            states.reserve(clients.size());
            for (std::size_t index = 0; index < clients.size(); ++index) {
                states.emplace_back(
                    clients[index].get(),
                    workload.requests_by_client[index],
                    outstanding_window);
            }

            std::vector<pollfd> poll_events(clients.size());
            std::vector<Latency> latencies;
            if (collect_latencies) {
                latencies.reserve(workload.command_count);
            }

            std::size_t total_sent = 0;
            std::size_t total_completed = 0;
            std::size_t total_outstanding = 0;
            std::size_t maximum_total_outstanding = 0;
            std::size_t maximum_per_client_outstanding = 0;
            auto last_progress = Clock::now();
            const auto start = last_progress;
            Clock::time_point completion_time{};

            const auto send_available = [&](ClientPhaseState& state) {
                while (state.next_request < state.requests->size() &&
                       state.outstanding.size() < outstanding_window) {
                    const std::string& request =
                        state.requests->at(state.next_request);
                    const char* const pending =
                        request.data() + state.request_offset;
                    const std::size_t pending_size =
                        request.size() - state.request_offset;
                    const ssize_t bytes_sent = ::send(
                        state.fd,
                        pending,
                        pending_size,
                        MSG_NOSIGNAL);

                    if (bytes_sent > 0) {
                        state.request_offset +=
                            static_cast<std::size_t>(bytes_sent);
                        last_progress = Clock::now();
                        if (state.request_offset == request.size()) {
                            state.request_offset = 0;
                            ++state.next_request;
                            ++state.sent_commands;
                            ++total_sent;

                            // A request enters the FIFO only when send() has
                            // accepted its final byte. A partial command is
                            // never counted as outstanding.
                            state.outstanding.push(
                                collect_latencies
                                    ? Clock::now()
                                    : Clock::time_point{});
                            ++total_outstanding;
                            state.maximum_outstanding = std::max(
                                state.maximum_outstanding,
                                state.outstanding.size());
                            maximum_per_client_outstanding = std::max(
                                maximum_per_client_outstanding,
                                state.maximum_outstanding);
                            maximum_total_outstanding = std::max(
                                maximum_total_outstanding,
                                total_outstanding);
                        }
                        continue;
                    }
                    if (bytes_sent == -1 && errno == EINTR) {
                        continue;
                    }
                    if (bytes_sent == -1 &&
                        (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        return;
                    }
                    throw std::system_error(
                        bytes_sent == -1 ? errno : EIO,
                        std::generic_category(),
                        "client send");
                }
            };

            const auto receive_available = [&](ClientPhaseState& state) {
                std::array<char, kReceiveBufferSize> buffer{};
                while (true) {
                    const ssize_t bytes_received = ::recv(
                        state.fd, buffer.data(), buffer.size(), 0);
                    if (bytes_received > 0) {
                        const std::size_t previous_completed =
                            state.parser.completed_responses();
                        state.parser.append(std::string_view{
                            buffer.data(),
                            static_cast<std::size_t>(bytes_received)});
                        const std::size_t newly_completed =
                            state.parser.completed_responses() -
                            previous_completed;
                        if (newly_completed > state.outstanding.size()) {
                            throw std::runtime_error(
                                "received response without an outstanding request");
                        }

                        for (std::size_t completed = 0;
                             completed < newly_completed;
                             ++completed) {
                            const Clock::time_point sent_at =
                                state.outstanding.pop();
                            --total_outstanding;
                            ++state.completed_responses;
                            ++total_completed;
                            if (collect_latencies) {
                                latencies.push_back(
                                    std::chrono::duration_cast<Latency>(
                                        Clock::now() - sent_at));
                            }
                        }
                        last_progress = Clock::now();

                        if (state.parser.completed_responses() >
                            state.requests->size() ||
                            state.completed_responses >
                            state.requests->size() ||
                            total_completed > workload.command_count) {
                            throw std::runtime_error(
                                "received more responses than commands");
                        }
                        if (total_completed == workload.command_count) {
                            completion_time = Clock::now();
                            return;
                        }
                        continue;
                    }
                    if (bytes_received == 0) {
                        throw std::runtime_error(
                            "gateway closed a client connection before completion");
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return;
                    }
                    throw std::system_error(
                        errno, std::generic_category(), "client recv");
                }
            };

            for (ClientPhaseState& state : states) {
                send_available(state);
            }

            while (total_completed < workload.command_count) {
                gateway.rethrow_if_stopped();

                for (std::size_t index = 0;
                     index < states.size();
                     ++index) {
                    const ClientPhaseState& state = states[index];
                    poll_events[index] = pollfd{state.fd, POLLIN, 0};
                    if (state.request_offset != 0 ||
                        (state.next_request < state.requests->size() &&
                         state.outstanding.size() < outstanding_window)) {
                        poll_events[index].events |= POLLOUT;
                    }
                }

                int ready = -1;
                do {
                    ready = ::poll(
                        poll_events.data(),
                        static_cast<nfds_t>(poll_events.size()),
                        kPollIntervalMilliseconds);
                } while (ready == -1 && errno == EINTR);
                if (ready == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "client poll");
                }

                for (std::size_t index = 0;
                     index < states.size();
                     ++index) {
                    ClientPhaseState& state = states[index];
                    const short events = poll_events[index].revents;
                    if ((events & POLLNVAL) != 0) {
                        throw std::runtime_error(
                            "client poll reported POLLNVAL");
                    }
                    if ((events & POLLERR) != 0) {
                        throw std::system_error(
                            socket_error(state.fd),
                            std::generic_category(),
                            "client socket error");
                    }
                    if ((events & POLLOUT) != 0) {
                        send_available(state);
                    }
                    if ((events & POLLIN) != 0) {
                        receive_available(state);
                    }
                    if ((events & POLLHUP) != 0) {
                        receive_available(state);
                        throw std::runtime_error(
                            "gateway unexpectedly closed a client connection");
                    }
                }

                gateway.rethrow_if_stopped();
                if (Clock::now() - last_progress > kNoProgressTimeout) {
                    throw std::runtime_error(
                        "gateway benchmark timed out without progress");
                }
            }

            if (completion_time == Clock::time_point{}) {
                completion_time = Clock::now();
            }

            std::size_t observed_trade_count = 0;
            for (const ClientPhaseState& state : states) {
                if (state.sent_commands != state.requests->size() ||
                    state.next_request != state.requests->size() ||
                    state.request_offset != 0 ||
                    !state.outstanding.empty() ||
                    state.completed_responses != state.requests->size() ||
                    state.parser.completed_responses() !=
                        state.requests->size()) {
                    throw std::runtime_error(
                        "per-client command/response accounting mismatch");
                }
                if (!state.parser.idle()) {
                    throw std::runtime_error(
                        "incomplete response bytes remain after phase");
                }
                if (state.maximum_outstanding > outstanding_window) {
                    throw std::logic_error(
                        "per-client outstanding window exceeded");
                }
                observed_trade_count += state.parser.trade_count();
            }

            if (total_sent != workload.command_count ||
                total_completed != workload.command_count ||
                total_outstanding != 0) {
                throw std::runtime_error(
                    "global command/response accounting mismatch");
            }
            if (observed_trade_count != workload.expected_trade_count) {
                throw std::runtime_error("trade count mismatch");
            }
            if (maximum_total_outstanding >
                clients.size() * outstanding_window) {
                throw std::logic_error(
                    "global outstanding window exceeded");
            }
            if (collect_latencies &&
                latencies.size() != workload.command_count) {
                throw std::runtime_error("latency sample count mismatch");
            }

            return PhaseResult{
                total_sent,
                total_completed,
                observed_trade_count,
                maximum_per_client_outstanding,
                maximum_total_outstanding,
                completion_time - start,
                std::move(latencies)};
        }

        void validate_phase(
            const PhaseResult& result,
            const Workload& workload,
            std::size_t client_count,
            std::size_t outstanding_window,
            bool expect_latencies,
            std::string_view phase_name) {
            if (result.sent_commands != workload.command_count ||
                result.completed_responses != workload.command_count ||
                result.trade_count != workload.expected_trade_count ||
                result.maximum_per_client_outstanding > outstanding_window ||
                result.maximum_total_outstanding >
                    client_count * outstanding_window ||
                (expect_latencies &&
                 result.latencies.size() != workload.command_count) ||
                (!expect_latencies && !result.latencies.empty())) {
                throw std::runtime_error(
                    std::string{phase_name} + " validation failed");
            }
        }

        [[nodiscard]] PhaseResult run_repetition(
            const BenchmarkConfig& config,
            std::size_t client_count) {
            const Workload warmup = make_paired_cross_workload(
                config.warmup_command_count, 1, 1, client_count);

            const OrderId measured_first_id =
                static_cast<OrderId>(config.warmup_command_count) + 1;
            const Timestamp measured_first_timestamp =
                static_cast<Timestamp>(config.warmup_command_count) + 1;
            const Workload measured = make_paired_cross_workload(
                config.command_count,
                measured_first_id,
                measured_first_timestamp,
                client_count);

            const std::size_t outstanding_window =
                scenario_window(config.scenario);
            const bool collect_latencies =
                config.scenario == Scenario::Latency;

            RunningGateway gateway;
            const std::vector<ScopedFd> clients =
                connect_clients(client_count, gateway.local_port());

            const PhaseResult warmup_result = run_phase(
                clients,
                warmup,
                outstanding_window,
                false,
                gateway);
            validate_phase(
                warmup_result,
                warmup,
                client_count,
                outstanding_window,
                false,
                "warmup");

            PhaseResult measured_result = run_phase(
                clients,
                measured,
                outstanding_window,
                collect_latencies,
                gateway);
            validate_phase(
                measured_result,
                measured,
                client_count,
                outstanding_window,
                collect_latencies,
                "measured");

            gateway.stop_and_check();
            return measured_result;
        }

        struct CommandPressureResult {
            Clock::duration elapsed{};
            std::size_t burst_commands{};
        };

        [[nodiscard]] CommandPressureResult run_command_pressure_once(
            std::size_t burst_command_count) {
            constexpr OrderId kSentinelSellId = 4'000'000'000;
            constexpr OrderId kHealthyBuyId = 5'000'000'000;
            constexpr OrderId kHealthyRestingId = 5'000'000'001;
            constexpr Price kSentinelPrice = 500'000;
            constexpr Quantity kSentinelQuantity = 1;
            constexpr Timestamp kSentinelTimestamp = 1;
            constexpr Timestamp kHealthyBuyTimestamp = 2;
            constexpr Timestamp kHealthyRestingTimestamp = 3;

            const Clock::time_point started_at = Clock::now();
            const Clock::time_point deadline =
                started_at + kNoProgressTimeout;
            RunningGateway gateway{
                kPressureQueueCapacity,
                kPressureQueueCapacity};
            const ScopedFd offending_client =
                connect_client(gateway.local_port());
            const ScopedFd healthy_client =
                connect_client(gateway.local_port());

            std::string burst = make_add_request(
                kSentinelSellId,
                "SELL",
                kSentinelPrice,
                kSentinelQuantity,
                kSentinelTimestamp);
            burst.reserve(burst.size() + 8 * burst_command_count);
            for (std::size_t index = 1;
                 index < burst_command_count;
                 ++index) {
                burst += "BROKEN\n";
            }

            if (!send_before_deadline(
                    offending_client.get(), burst, deadline, gateway)) {
                throw std::runtime_error(
                    "offending client closed before burst submission completed");
            }
            wait_for_peer_close_and_discard(
                offending_client.get(), deadline, gateway);

            // Give the response queue wakeup a safe event-loop boundary so
            // all already-admitted offender work can finish before checking
            // the unaffected connection.
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            gateway.rethrow_if_stopped();

            CapturedResponseParser healthy_parser;
            const std::string healthy_buy = make_add_request(
                kHealthyBuyId,
                "BUY",
                kSentinelPrice,
                kSentinelQuantity,
                kHealthyBuyTimestamp);
            if (!send_before_deadline(
                    healthy_client.get(), healthy_buy, deadline, gateway)) {
                throw std::runtime_error(
                    "healthy client closed after command admission overload");
            }
            expect_response_before_deadline(
                healthy_client.get(),
                healthy_parser,
                full_match_response(
                    kHealthyBuyId,
                    kSentinelSellId,
                    kSentinelPrice,
                    kSentinelQuantity,
                    kHealthyBuyTimestamp),
                deadline,
                gateway);

            const std::string healthy_remainder = make_add_request(
                kHealthyRestingId,
                "SELL",
                kSentinelPrice + 1,
                kSentinelQuantity,
                kHealthyRestingTimestamp);
            if (!send_before_deadline(
                    healthy_client.get(),
                    healthy_remainder,
                    deadline,
                    gateway)) {
                throw std::runtime_error(
                    "healthy client stopped responding after overload");
            }
            expect_response_before_deadline(
                healthy_client.get(),
                healthy_parser,
                accepted_response(
                    kHealthyRestingId,
                    "SELL",
                    kSentinelPrice + 1,
                    kSentinelQuantity,
                    kHealthyRestingTimestamp),
                deadline,
                gateway);

            gateway.rethrow_if_stopped();
            gateway.stop_and_check();
            return CommandPressureResult{
                Clock::now() - started_at,
                burst_command_count};
        }

        struct SlowReaderResult {
            Clock::duration elapsed{};
            std::size_t submitted_slow_commands{};
            Quantity applied_slow_fills{};
            std::size_t healthy_responses{};
        };

        [[nodiscard]] SlowReaderResult run_slow_reader_once(
            std::size_t maximum_slow_commands) {
            constexpr int kSlowReceiveBufferBytes = 1024;
            constexpr OrderId kSentinelSellId = 6'000'000'000;
            constexpr OrderId kFirstSlowBuyId = 6'000'000'001;
            constexpr OrderId kFirstHealthyOrderId = 7'000'000'000;
            constexpr Price kSentinelPrice = 1'000'000;
            constexpr Price kHealthyPrice = 100;
            constexpr Quantity kSentinelQuantity = 2'000'000;
            constexpr Quantity kFillQuantity = 1;
            constexpr Timestamp kSentinelTimestamp = 1;

            const Clock::time_point started_at = Clock::now();
            const Clock::time_point deadline =
                started_at + kNoProgressTimeout;
            RunningGateway gateway;
            const ScopedFd slow_client = connect_client(
                gateway.local_port(), kSlowReceiveBufferBytes);
            const ScopedFd healthy_client =
                connect_client(gateway.local_port());
            CapturedResponseParser healthy_parser;

            const std::string sentinel = make_add_request(
                kSentinelSellId,
                "SELL",
                kSentinelPrice,
                kSentinelQuantity,
                kSentinelTimestamp);
            if (!send_before_deadline(
                    slow_client.get(), sentinel, deadline, gateway)) {
                throw std::runtime_error(
                    "slow client closed before sentinel admission");
            }

            std::size_t submitted_slow_commands = 1;
            std::size_t submitted_slow_buys = 0;
            std::size_t healthy_responses = 0;
            std::size_t healthy_pair_index = 0;
            bool slow_client_closed = false;

            while (submitted_slow_commands < maximum_slow_commands &&
                   !slow_client_closed) {
                const std::size_t batch_count = std::min(
                    kSlowReaderBatchSize,
                    maximum_slow_commands - submitted_slow_commands);
                std::string slow_batch;
                slow_batch.reserve(batch_count * 48);
                for (std::size_t index = 0;
                     index < batch_count;
                     ++index) {
                    const OrderId order_id =
                        kFirstSlowBuyId + submitted_slow_buys + index;
                    const Timestamp timestamp = static_cast<Timestamp>(
                        2 + submitted_slow_buys + index);
                    slow_batch += make_add_request(
                        order_id,
                        "BUY",
                        kSentinelPrice,
                        kFillQuantity,
                        timestamp);
                }

                if (!send_before_deadline(
                        slow_client.get(), slow_batch, deadline, gateway)) {
                    slow_client_closed = true;
                    break;
                }
                submitted_slow_commands += batch_count;
                submitted_slow_buys += batch_count;

                // This pacing keeps command admission far below the normal
                // 1024-entry queue capacity. The stress variable is that the
                // slow client never reads its responses.
                std::this_thread::sleep_for(std::chrono::milliseconds{1});

                const OrderId healthy_sell_id =
                    kFirstHealthyOrderId + 2 * healthy_pair_index;
                const OrderId healthy_buy_id = healthy_sell_id + 1;
                const Timestamp healthy_sell_timestamp =
                    static_cast<Timestamp>(
                        1'000'000 + 2 * healthy_pair_index);
                const Timestamp healthy_buy_timestamp =
                    healthy_sell_timestamp + 1;
                std::string healthy_pair = make_add_request(
                    healthy_sell_id,
                    "SELL",
                    kHealthyPrice,
                    kFillQuantity,
                    healthy_sell_timestamp);
                healthy_pair += make_add_request(
                    healthy_buy_id,
                    "BUY",
                    kHealthyPrice,
                    kFillQuantity,
                    healthy_buy_timestamp);
                if (!send_before_deadline(
                        healthy_client.get(),
                        healthy_pair,
                        deadline,
                        gateway)) {
                    throw std::runtime_error(
                        "healthy client closed during slow-reader stress");
                }

                expect_response_before_deadline(
                    healthy_client.get(),
                    healthy_parser,
                    accepted_response(
                        healthy_sell_id,
                        "SELL",
                        kHealthyPrice,
                        kFillQuantity,
                        healthy_sell_timestamp),
                    deadline,
                    gateway);
                expect_response_before_deadline(
                    healthy_client.get(),
                    healthy_parser,
                    full_match_response(
                        healthy_buy_id,
                        healthy_sell_id,
                        kHealthyPrice,
                        kFillQuantity,
                        healthy_buy_timestamp),
                    deadline,
                    gateway);
                healthy_responses += 2;
                ++healthy_pair_index;

                slow_client_closed =
                    peer_close_signaled(slow_client.get());
            }

            if (!slow_client_closed) {
                throw std::runtime_error(
                    "slow client did not reach the pending-output limit "
                    "before the configured command limit");
            }

            const std::string cancel_request =
                "CANCEL " + std::to_string(kSentinelSellId) + "\n";
            if (!send_before_deadline(
                    healthy_client.get(),
                    cancel_request,
                    deadline,
                    gateway)) {
                throw std::runtime_error(
                    "healthy client closed before state verification");
            }
            const std::string cancel_response =
                receive_response_before_deadline(
                    healthy_client.get(),
                    healthy_parser,
                    deadline,
                    gateway);
            ++healthy_responses;
            const Quantity remaining_quantity =
                cancelled_remaining_quantity(
                    cancel_response,
                    kSentinelSellId,
                    kSentinelPrice,
                    kSentinelTimestamp);
            if (remaining_quantity <= 0 ||
                remaining_quantity >= kSentinelQuantity) {
                throw std::runtime_error(
                    "slow-client matching state was not preserved");
            }

            const Quantity applied_slow_fills =
                kSentinelQuantity - remaining_quantity;
            if (applied_slow_fills >
                static_cast<Quantity>(submitted_slow_buys)) {
                throw std::runtime_error(
                    "sentinel state exceeds submitted slow-client work");
            }

            gateway.rethrow_if_stopped();
            gateway.stop_and_check();
            return SlowReaderResult{
                Clock::now() - started_at,
                submitted_slow_commands,
                applied_slow_fills,
                healthy_responses};
        }

        [[nodiscard]] double elapsed_seconds(const PhaseResult& result) {
            return std::chrono::duration<double>(result.elapsed).count();
        }

        [[nodiscard]] double elapsed_seconds(Clock::duration elapsed) {
            return std::chrono::duration<double>(elapsed).count();
        }

        [[nodiscard]] double median(std::vector<double> values) {
            if (values.empty()) {
                throw std::invalid_argument("median requires values");
            }
            std::sort(values.begin(), values.end());
            const std::size_t middle = values.size() / 2;
            if (values.size() % 2 != 0) {
                return values[middle];
            }
            return (values[middle - 1] + values[middle]) / 2.0;
        }

        [[nodiscard]] double coefficient_of_variation_percent(
            const std::vector<double>& values) {
            if (values.size() < 2) {
                return 0.0;
            }

            double sum = 0.0;
            for (const double value : values) {
                sum += value;
            }
            const double mean = sum / static_cast<double>(values.size());

            double squared_difference_sum = 0.0;
            for (const double value : values) {
                const double difference = value - mean;
                squared_difference_sum += difference * difference;
            }
            const double sample_standard_deviation = std::sqrt(
                squared_difference_sum /
                static_cast<double>(values.size() - 1));
            return sample_standard_deviation / mean * 100.0;
        }

        [[nodiscard]] Latency nearest_rank_percentile(
            const std::vector<Latency>& sorted_samples,
            std::size_t numerator,
            std::size_t denominator) {
            if (sorted_samples.empty() || numerator == 0 ||
                numerator > denominator) {
                throw std::invalid_argument(
                    "invalid nearest-rank percentile input");
            }
            const std::size_t rank =
                (sorted_samples.size() * numerator + denominator - 1) /
                denominator;
            return sorted_samples[rank - 1];
        }

        [[nodiscard]] LatencySummary summarize_latencies(
            std::vector<Latency>& samples) {
            if (samples.empty()) {
                throw std::invalid_argument(
                    "latency summary requires samples");
            }
            std::sort(samples.begin(), samples.end());
            return LatencySummary{
                samples.size(),
                nearest_rank_percentile(samples, 50, 100),
                nearest_rank_percentile(samples, 95, 100),
                nearest_rank_percentile(samples, 99, 100),
                samples.back()};
        }

        [[nodiscard]] double microseconds(Latency latency) {
            return static_cast<double>(latency.count()) / 1'000.0;
        }

        void print_metadata(const BenchmarkConfig& config) {
            std::cout
                << "scenario: " << scenario_name(config.scenario) << '\n'
                << "clients: ";
            for (std::size_t index = 0;
                 index < config.client_counts.size();
                 ++index) {
                if (index != 0) {
                    std::cout << ',';
                }
                std::cout << config.client_counts[index];
            }
            std::cout
                << '\n'
                << "workload: ";
            switch (config.scenario) {
                case Scenario::Throughput:
                case Scenario::Latency:
                    std::cout << kWorkloadName;
                    break;
                case Scenario::CommandPressure:
                    std::cout << kCommandPressureWorkloadName;
                    break;
                case Scenario::SlowReader:
                    std::cout << kSlowReaderWorkloadName;
                    break;
            }
            std::cout
                << '\n'
                << "commands_per_repetition: " << config.command_count << '\n'
                << "warmup_commands_per_repetition: "
                << config.warmup_command_count << '\n'
                << "repetitions: " << config.repetitions << '\n';

            if (is_performance_scenario(config.scenario)) {
                std::cout
                    << "outstanding_window: "
                    << scenario_window(config.scenario) << '\n'
                    << "command_queue_capacity: "
                    << kDefaultCommandQueueCapacity << '\n'
                    << "response_queue_capacity: "
                    << kDefaultResponseQueueCapacity << '\n'
                    << "warmup_order_id_range: 1-"
                    << config.warmup_command_count << '\n'
                    << "measured_order_id_range: "
                    << config.warmup_command_count + 1 << '-'
                    << config.warmup_command_count + config.command_count << '\n'
                    << "percentile_algorithm: nearest-rank\n"
                    << "cv_definition: sample_standard_deviation/mean\n";
                return;
            }

            if (config.scenario == Scenario::CommandPressure) {
                std::cout
                    << "outstanding_window: not-applicable (complete burst)\n"
                    << "command_queue_capacity: "
                    << kPressureQueueCapacity << '\n'
                    << "response_queue_capacity: "
                    << kPressureQueueCapacity << '\n';
            } else {
                std::cout
                    << "outstanding_window: not-applicable "
                       "(responses intentionally unread)\n"
                    << "controlled_submission_batch: "
                    << kSlowReaderBatchSize << '\n'
                    << "pending_output_limit_bytes: "
                    << kMaxPendingOutputBytes << '\n'
                    << "command_queue_capacity: "
                    << kDefaultCommandQueueCapacity << '\n'
                    << "response_queue_capacity: "
                    << kDefaultResponseQueueCapacity << '\n';
            }
            std::cout << "watchdog_seconds: "
                      << kNoProgressTimeout.count() << '\n';
        }

        void run_command_pressure_scenario(
            const BenchmarkConfig& config) {
            for (std::size_t repetition = 1;
                 repetition <= config.repetitions;
                 ++repetition) {
                const CommandPressureResult result =
                    run_command_pressure_once(config.command_count);
                std::cout
                    << std::fixed << std::setprecision(6)
                    << "run scenario=command-pressure repetition="
                    << repetition
                    << " elapsed_seconds="
                    << elapsed_seconds(result.elapsed)
                    << " burst_commands=" << result.burst_commands
                    << " offending_client_closed=PASS"
                    << " healthy_client_isolation=PASS"
                    << " admitted_state_preserved=PASS"
                    << " response_routing=PASS"
                    << " gateway_alive=PASS"
                    << " validation=PASS\n";
            }
        }

        void run_slow_reader_scenario(const BenchmarkConfig& config) {
            for (std::size_t repetition = 1;
                 repetition <= config.repetitions;
                 ++repetition) {
                const SlowReaderResult result =
                    run_slow_reader_once(config.command_count);
                std::cout
                    << std::fixed << std::setprecision(6)
                    << "run scenario=slow-reader repetition="
                    << repetition
                    << " elapsed_seconds="
                    << elapsed_seconds(result.elapsed)
                    << " submitted_slow_commands="
                    << result.submitted_slow_commands
                    << " applied_slow_fills="
                    << result.applied_slow_fills
                    << " healthy_responses="
                    << result.healthy_responses
                    << " slow_client_closed=PASS"
                    << " healthy_client_isolation=PASS"
                    << " admitted_state_preserved=PASS"
                    << " response_routing=PASS"
                    << " gateway_alive=PASS"
                    << " validation=PASS\n";
            }
        }

        void run_throughput_case(
            const BenchmarkConfig& config,
            std::size_t client_count) {
            std::vector<double> elapsed_values;
            std::vector<double> command_rates;
            std::vector<double> trade_rates;
            elapsed_values.reserve(config.repetitions);
            command_rates.reserve(config.repetitions);
            trade_rates.reserve(config.repetitions);

            std::size_t maximum_per_client_outstanding = 0;
            std::size_t maximum_total_outstanding = 0;

            for (std::size_t repetition = 1;
                 repetition <= config.repetitions;
                 ++repetition) {
                PhaseResult result;
                try {
                    result = run_repetition(config, client_count);
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        "throughput case failed for clients=" +
                        std::to_string(client_count) +
                        " repetition=" + std::to_string(repetition) +
                        ": " + error.what());
                }

                const double seconds = elapsed_seconds(result);
                const double commands_per_second =
                    static_cast<double>(result.completed_responses) / seconds;
                const double trades_per_second =
                    static_cast<double>(result.trade_count) / seconds;
                elapsed_values.push_back(seconds);
                command_rates.push_back(commands_per_second);
                trade_rates.push_back(trades_per_second);
                maximum_per_client_outstanding = std::max(
                    maximum_per_client_outstanding,
                    result.maximum_per_client_outstanding);
                maximum_total_outstanding = std::max(
                    maximum_total_outstanding,
                    result.maximum_total_outstanding);

                std::cout << std::fixed << std::setprecision(6)
                          << "run scenario=throughput clients="
                          << client_count
                          << " repetition=" << repetition
                          << " elapsed_seconds=" << seconds
                          << std::setprecision(2)
                          << " commands_per_second="
                          << commands_per_second
                          << " trades_per_second="
                          << trades_per_second
                          << " max_per_client_outstanding="
                          << result.maximum_per_client_outstanding
                          << " max_total_outstanding="
                          << result.maximum_total_outstanding
                          << " validation=PASS\n";
            }

            std::cout << "\n| Clients | Window | Commands | Median elapsed (s) | "
                         "Commands/s | Trades/s | CV |\n"
                      << "|---:|---:|---:|---:|---:|---:|---:|\n"
                      << "| " << client_count
                      << " | " << kThroughputOutstandingWindow
                      << " | " << config.command_count
                      << std::fixed << std::setprecision(6)
                      << " | " << median(elapsed_values)
                      << std::setprecision(2)
                      << " | " << median(command_rates)
                      << " | " << median(trade_rates)
                      << " | "
                      << coefficient_of_variation_percent(command_rates)
                      << "% |\n"
                      << "outstanding_validation clients=" << client_count
                      << " max_per_client="
                      << maximum_per_client_outstanding
                      << " max_total=" << maximum_total_outstanding
                      << " configured_total_limit="
                      << client_count * kThroughputOutstandingWindow
                      << "\n\n";
        }

        void run_latency_case(
            const BenchmarkConfig& config,
            std::size_t client_count) {
            std::vector<Latency> pooled_latencies;
            pooled_latencies.reserve(
                config.command_count * config.repetitions);
            std::size_t maximum_per_client_outstanding = 0;
            std::size_t maximum_total_outstanding = 0;

            for (std::size_t repetition = 1;
                 repetition <= config.repetitions;
                 ++repetition) {
                PhaseResult result;
                try {
                    result = run_repetition(config, client_count);
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        "latency case failed for clients=" +
                        std::to_string(client_count) +
                        " repetition=" + std::to_string(repetition) +
                        ": " + error.what());
                }

                maximum_per_client_outstanding = std::max(
                    maximum_per_client_outstanding,
                    result.maximum_per_client_outstanding);
                maximum_total_outstanding = std::max(
                    maximum_total_outstanding,
                    result.maximum_total_outstanding);

                LatencySummary repetition_summary =
                    summarize_latencies(result.latencies);
                std::cout << std::fixed << std::setprecision(3)
                          << "run scenario=latency clients="
                          << client_count
                          << " repetition=" << repetition
                          << " samples="
                          << repetition_summary.sample_count
                          << " p50_us="
                          << microseconds(repetition_summary.p50)
                          << " p95_us="
                          << microseconds(repetition_summary.p95)
                          << " p99_us="
                          << microseconds(repetition_summary.p99)
                          << " max_us="
                          << microseconds(repetition_summary.maximum)
                          << " max_per_client_outstanding="
                          << result.maximum_per_client_outstanding
                          << " max_total_outstanding="
                          << result.maximum_total_outstanding
                          << " validation=PASS\n";

                pooled_latencies.insert(
                    pooled_latencies.end(),
                    result.latencies.begin(),
                    result.latencies.end());
            }

            const LatencySummary summary =
                summarize_latencies(pooled_latencies);
            std::cout << "\n| Clients | Model | Samples | p50 (us) | "
                         "p95 (us) | p99 (us) | Max (us) |\n"
                      << "|---:|---|---:|---:|---:|---:|---:|\n"
                      << "| " << client_count
                      << " | closed-loop | " << summary.sample_count
                      << std::fixed << std::setprecision(3)
                      << " | " << microseconds(summary.p50)
                      << " | " << microseconds(summary.p95)
                      << " | " << microseconds(summary.p99)
                      << " | " << microseconds(summary.maximum)
                      << " |\n"
                      << "outstanding_validation clients=" << client_count
                      << " max_per_client="
                      << maximum_per_client_outstanding
                      << " max_total=" << maximum_total_outstanding
                      << " configured_total_limit="
                      << client_count * kLatencyOutstandingWindow
                      << "\n\n";
        }

        int run_benchmark(const BenchmarkConfig& config) {
            print_metadata(config);
            std::cout << '\n';

            if (config.scenario == Scenario::CommandPressure) {
                run_command_pressure_scenario(config);
                return 0;
            }
            if (config.scenario == Scenario::SlowReader) {
                run_slow_reader_scenario(config);
                return 0;
            }

            for (const std::size_t client_count : config.client_counts) {
                if (config.scenario == Scenario::Throughput) {
                    run_throughput_case(config, client_count);
                } else {
                    run_latency_case(config, client_count);
                }
            }
            return 0;
        }
    }  // namespace
}  // namespace exchange

int main(int argc, char** argv) {
    try {
        return exchange::run_benchmark(exchange::parse_arguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "exchange_gateway_benchmark failed: "
                  << error.what() << '\n';
        return 1;
    }
}
