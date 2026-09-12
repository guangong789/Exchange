#include "durability/execution_wal.hpp"
#include "execution/trading_bootstrap.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#ifndef EXCHANGE_SERVER_PATH
#error "EXCHANGE_SERVER_PATH must name the production exchange_server executable"
#endif

namespace exchange {
    namespace {
        using namespace std::chrono_literals;

        constexpr InstrumentContext kServerInstrument{1, 2, 1, 1, 1};
        constexpr Amount kServerInitialBalance = 1'000'000'000;
        constexpr auto kOperationTimeout = 5s;

        TradingBootstrapConfig server_bootstrap() {
            return TradingBootstrapConfig{{
                BootstrapAccount{
                    1,
                    {{kServerInstrument.base_asset,
                      {kServerInitialBalance, 0}},
                     {kServerInstrument.quote_asset,
                      {kServerInitialBalance, 0}}}},
                BootstrapAccount{
                    2,
                    {{kServerInstrument.base_asset,
                      {kServerInitialBalance, 0}},
                     {kServerInstrument.quote_asset,
                      {kServerInitialBalance, 0}}}},
            }};
        }

        class ScopedFd {
        public:
            explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
            ~ScopedFd() {
                if (fd_ != -1) {
                    ::close(fd_);
                }
            }

            ScopedFd(const ScopedFd&) = delete;
            ScopedFd& operator=(const ScopedFd&) = delete;
            ScopedFd(ScopedFd&& other) noexcept
                : fd_(std::exchange(other.fd_, -1)) {}
            ScopedFd& operator=(ScopedFd&& other) noexcept {
                if (this != &other) {
                    if (fd_ != -1) {
                        ::close(fd_);
                    }
                    fd_ = std::exchange(other.fd_, -1);
                }
                return *this;
            }

            [[nodiscard]] int get() const noexcept { return fd_; }
            [[nodiscard]] int release() noexcept {
                return std::exchange(fd_, -1);
            }

        private:
            int fd_;
        };

        class TemporaryDirectory {
        public:
            TemporaryDirectory() {
                std::string pattern = "/tmp/exchange-process-test-XXXXXX";
                char* const created = ::mkdtemp(pattern.data());
                if (created == nullptr) {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "create process-test directory");
                }
                path_ = created;
                wal_path_ = path_ + "/exchange.wal";
            }

            ~TemporaryDirectory() {
                static_cast<void>(::unlink(wal_path_.c_str()));
                static_cast<void>(::rmdir(path_.c_str()));
            }

            TemporaryDirectory(const TemporaryDirectory&) = delete;
            TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

            [[nodiscard]] const std::string& wal_path() const noexcept {
                return wal_path_;
            }

        private:
            std::string path_;
            std::string wal_path_;
        };

        [[nodiscard]] std::uint16_t reserve_free_port() {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "create port probe");
            }
            ScopedFd socket{fd};

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            if (::bind(
                    fd,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "bind port probe");
            }

            socklen_t length = sizeof(address);
            if (::getsockname(
                    fd,
                    reinterpret_cast<sockaddr*>(&address),
                    &length) == -1) {
                throw std::system_error(
                    errno, std::generic_category(), "read probed port");
            }
            return ntohs(address.sin_port);
        }

        class ServerProcess {
        public:
            ServerProcess(std::uint16_t port, const std::string& wal_path)
                : port_(port) {
                int output_pipe[2]{};
                if (::pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "create server output pipe");
                }
                ScopedFd read_end{output_pipe[0]};
                ScopedFd write_end{output_pipe[1]};

                const pid_t child = ::fork();
                if (child == -1) {
                    throw std::system_error(
                        errno, std::generic_category(), "fork exchange_server");
                }
                if (child == 0) {
                    if (::dup2(write_end.get(), STDOUT_FILENO) == -1 ||
                        ::dup2(write_end.get(), STDERR_FILENO) == -1) {
                        _exit(126);
                    }
                    const std::string port_text = std::to_string(port_);
                    ::execl(
                        EXCHANGE_SERVER_PATH,
                        EXCHANGE_SERVER_PATH,
                        port_text.c_str(),
                        wal_path.c_str(),
                        static_cast<char*>(nullptr));
                    _exit(127);
                }

                pid_ = child;
                output_fd_ = ScopedFd{read_end.release()};
            }

            ~ServerProcess() {
                terminate_noexcept();
            }

            ServerProcess(const ServerProcess&) = delete;
            ServerProcess& operator=(const ServerProcess&) = delete;

            [[nodiscard]] ScopedFd connect_when_ready() {
                const auto deadline = std::chrono::steady_clock::now() +
                    kOperationTimeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    drain_output();
                    int status = 0;
                    const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
                    if (waited == pid_) {
                        pid_ = -1;
                        throw std::runtime_error(
                            "exchange_server exited before readiness; output:\n" +
                            output_);
                    }

                    const int fd =
                        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                    if (fd == -1) {
                        throw std::system_error(
                            errno, std::generic_category(), "create test client");
                    }
                    ScopedFd client{fd};
                    sockaddr_in address{};
                    address.sin_family = AF_INET;
                    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    address.sin_port = htons(port_);
                    if (::connect(
                            fd,
                            reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address)) == 0) {
                        return client;
                    }
                    if (errno != ECONNREFUSED && errno != EINTR) {
                        throw std::system_error(
                            errno, std::generic_category(), "connect to exchange_server");
                    }
                    std::this_thread::sleep_for(10ms);
                }
                throw std::runtime_error(
                    "exchange_server readiness timeout; output:\n" + output());
            }

            void crash() {
                if (pid_ == -1) {
                    return;
                }
                if (::kill(pid_, SIGKILL) == -1 && errno != ESRCH) {
                    throw std::system_error(
                        errno, std::generic_category(), "kill exchange_server");
                }
                wait_for_exit();
            }

            [[nodiscard]] std::string output() {
                drain_output();
                return output_;
            }

        private:
            void wait_for_exit() {
                const auto deadline = std::chrono::steady_clock::now() +
                    kOperationTimeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    int status = 0;
                    const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
                    if (waited == pid_ || (waited == -1 && errno == ECHILD)) {
                        pid_ = -1;
                        drain_output();
                        return;
                    }
                    if (waited == -1 && errno != EINTR) {
                        throw std::system_error(
                            errno, std::generic_category(), "wait for exchange_server");
                    }
                    std::this_thread::sleep_for(10ms);
                }
                throw std::runtime_error("exchange_server termination timeout");
            }

            void drain_output() {
                if (output_fd_.get() == -1) {
                    return;
                }
                std::array<char, 1024> buffer{};
                while (true) {
                    const ssize_t count =
                        ::read(output_fd_.get(), buffer.data(), buffer.size());
                    if (count > 0) {
                        output_.append(buffer.data(), static_cast<std::size_t>(count));
                        continue;
                    }
                    if (count == -1 && errno == EINTR) {
                        continue;
                    }
                    if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        return;
                    }
                    return;
                }
            }

            void terminate_noexcept() noexcept {
                if (pid_ != -1) {
                    static_cast<void>(::kill(pid_, SIGKILL));
                    int status = 0;
                    while (::waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
                    }
                    pid_ = -1;
                }
                try {
                    drain_output();
                } catch (...) {
                }
            }

            std::uint16_t port_{};
            pid_t pid_{-1};
            ScopedFd output_fd_;
            std::string output_;
        };

        class TcpClient {
        public:
            explicit TcpClient(ScopedFd socket) : socket_(std::move(socket)) {}

            void send_request(std::string_view request) {
                std::size_t sent = 0;
                while (sent < request.size()) {
                    const ssize_t count = ::send(
                        socket_.get(),
                        request.data() + sent,
                        request.size() - sent,
                        MSG_NOSIGNAL);
                    if (count > 0) {
                        sent += static_cast<std::size_t>(count);
                        continue;
                    }
                    if (count == -1 && errno == EINTR) {
                        continue;
                    }
                    throw std::system_error(
                        errno, std::generic_category(), "send test request");
                }
            }

            [[nodiscard]] std::vector<std::string> read_response() {
                std::vector<std::string> lines;
                lines.push_back(read_line());
                const std::string& header = lines.front();
                const std::size_t last_space = header.rfind(' ');
                if (last_space == std::string::npos) {
                    throw std::runtime_error("response header has no event count");
                }
                std::size_t event_count = 0;
                const std::string count_text = header.substr(last_space + 1);
                try {
                    event_count = static_cast<std::size_t>(std::stoull(count_text));
                } catch (...) {
                    throw std::runtime_error("invalid response event count");
                }
                for (std::size_t index = 0; index < event_count; ++index) {
                    lines.push_back(read_line());
                }
                return lines;
            }

            void wait_until_response_is_pending() const {
                pollfd event{socket_.get(), POLLIN, 0};
                int result = -1;
                do {
                    result = ::poll(
                        &event,
                        1,
                        static_cast<int>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                kOperationTimeout).count()));
                } while (result == -1 && errno == EINTR);
                if (result <= 0 || (event.revents & POLLIN) == 0) {
                    throw std::runtime_error(
                        "response did not become pending before timeout");
                }
            }

        private:
            [[nodiscard]] std::string read_line() {
                const auto deadline = std::chrono::steady_clock::now() +
                    kOperationTimeout;
                while (true) {
                    const std::size_t newline = buffer_.find('\n');
                    if (newline != std::string::npos) {
                        std::string line = buffer_.substr(0, newline);
                        buffer_.erase(0, newline + 1);
                        return line;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        throw std::runtime_error("response line timeout");
                    }
                    const int remaining_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now).count());
                    pollfd event{socket_.get(), POLLIN, 0};
                    int result = ::poll(&event, 1, remaining_ms);
                    if (result == -1 && errno == EINTR) {
                        continue;
                    }
                    if (result <= 0 || (event.revents & (POLLIN | POLLHUP)) == 0) {
                        throw std::runtime_error("response poll timeout");
                    }

                    std::array<char, 4096> bytes{};
                    const ssize_t count =
                        ::recv(socket_.get(), bytes.data(), bytes.size(), 0);
                    if (count > 0) {
                        buffer_.append(bytes.data(), static_cast<std::size_t>(count));
                        continue;
                    }
                    if (count == -1 && errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error("server closed before complete response");
                }
            }

            ScopedFd socket_;
            std::string buffer_;
        };

        [[nodiscard]] WalScanResult scan_wal(const std::string& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("open process-test WAL");
            }
            const std::vector<std::uint8_t> bytes{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}};
            return scan_execution_wal(
                bytes,
                kServerInstrument,
                calculate_bootstrap_fingerprint(server_bootstrap()));
        }

        TEST(DurableServerProcessTest,
             CrashRestartPreservesStateAndContinuesAllSequences) {
            TemporaryDirectory directory;
            ServerProcess first{reserve_free_port(), directory.wal_path()};
            TcpClient client{first.connect_when_ready()};
            SCOPED_TRACE(first.output());

            client.send_request("ADD 1 1 SELL 100 3\n");
            EXPECT_EQ(
                client.read_response(),
                (std::vector<std::string>{
                    "RESULT 1 ACCEPTED 1 1",
                    "EVENT ORDER_ACCEPTED 1 SELL 100 3 1"}));

            client.send_request("ADD 2 2 BUY 100 2\n");
            const std::vector<std::string> trade = client.read_response();
            ASSERT_EQ(trade.size(), 5U) << first.output();
            EXPECT_EQ(trade[0], "RESULT 2 ACCEPTED 2 4");
            EXPECT_EQ(trade[1], "EVENT ORDER_ACCEPTED 2 BUY 100 2 2");
            EXPECT_EQ(trade[2], "EVENT TRADE_CREATED 2 1 100 2 2");

            client.send_request("ADD 3 2 BUY 1000000000 2\n");
            EXPECT_EQ(
                client.read_response(),
                (std::vector<std::string>{
                    "RESULT 3 INSUFFICIENT_FUNDS 0 0"}));
            first.crash();

            ServerProcess restarted{reserve_free_port(), directory.wal_path()};
            TcpClient recovered_client{restarted.connect_when_ready()};
            SCOPED_TRACE(restarted.output());
            recovered_client.send_request("ADD 4 2 BUY 100 1\n");
            const std::vector<std::string> recovered_trade =
                recovered_client.read_response();
            ASSERT_EQ(recovered_trade.size(), 5U) << restarted.output();
            EXPECT_EQ(recovered_trade[0], "RESULT 4 ACCEPTED 4 4");
            EXPECT_EQ(
                recovered_trade[1],
                "EVENT ORDER_ACCEPTED 4 BUY 100 1 4");
            EXPECT_EQ(
                recovered_trade[2],
                "EVENT TRADE_CREATED 4 1 100 1 4");
            restarted.crash();

            const WalScanResult scan = scan_wal(directory.wal_path());
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 4U);
            for (std::size_t index = 0; index < scan.records.size(); ++index) {
                EXPECT_EQ(scan.records[index].sequence, index + 1);
            }
            const auto* rejected_submit = std::get_if<SubmitExecutionCommand>(
                &scan.records[2].command);
            ASSERT_NE(rejected_submit, nullptr);
            EXPECT_EQ(rejected_submit->request_id, 3U);
            EXPECT_EQ(rejected_submit->order.id, 3U);
            EXPECT_EQ(rejected_submit->order.timestamp, 3);
        }

        TEST(DurableServerProcessTest,
             UnreadDurableResponseIsRecoveredAndRetryIsANewAttempt) {
            TemporaryDirectory directory;
            ServerProcess first{reserve_free_port(), directory.wal_path()};
            TcpClient client{first.connect_when_ready()};
            SCOPED_TRACE(first.output());

            client.send_request("ADD 40 1 SELL 90 1\n");
            client.wait_until_response_is_pending();
            first.crash();

            ServerProcess restarted{reserve_free_port(), directory.wal_path()};
            TcpClient recovered_client{restarted.connect_when_ready()};
            SCOPED_TRACE(restarted.output());
            recovered_client.send_request("ADD 41 2 BUY 90 1\n");
            const std::vector<std::string> recovered_trade =
                recovered_client.read_response();
            ASSERT_EQ(recovered_trade.size(), 5U) << restarted.output();
            EXPECT_EQ(recovered_trade[0], "RESULT 41 ACCEPTED 2 4");
            EXPECT_EQ(
                recovered_trade[2],
                "EVENT TRADE_CREATED 2 1 90 1 2");

            // RequestId is correlation only. Retrying the same operation after
            // an ambiguous disconnect creates a new execution attempt.
            recovered_client.send_request("ADD 40 1 SELL 90 1\n");
            EXPECT_EQ(
                recovered_client.read_response(),
                (std::vector<std::string>{
                    "RESULT 40 ACCEPTED 3 1",
                    "EVENT ORDER_ACCEPTED 3 SELL 90 1 3"}));
            restarted.crash();

            const WalScanResult scan = scan_wal(directory.wal_path());
            ASSERT_EQ(scan.status, WalScanStatus::CleanEof);
            ASSERT_EQ(scan.records.size(), 3U);
            EXPECT_EQ(scan.records[0].sequence, 1U);
            EXPECT_EQ(scan.records[1].sequence, 2U);
            EXPECT_EQ(scan.records[2].sequence, 3U);
        }
    }  // namespace
}  // namespace exchange
