#include "gateway/tcp_gateway.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "protocol/line_protocol.hpp"

namespace exchange {
    TcpGateway::TcpGateway(
        std::uint16_t port,
        std::unique_ptr<TradingRuntime> runtime,
        std::size_t command_queue_capacity,
        std::size_t response_queue_capacity)
        : runtime_(std::move(runtime)),
          command_queue_(command_queue_capacity),
          response_queue_(response_queue_capacity),
          server_(
              port,
              [this](
                  ConnectionId connection_id,
                  int,
                  std::string_view line) {
                  handle_line(connection_id, line);
              },
              [this] { handle_wakeup(); }) {
        if (!runtime_) {
            throw std::invalid_argument("trading runtime must not be null");
        }
        execution_thread_ = std::thread(&TcpGateway::execution_loop, this);
    }

    TcpGateway::~TcpGateway() {
        request_stop();
        if (execution_thread_.joinable()) {
            execution_thread_.join();
        }
    }

    void TcpGateway::run() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            rethrow_worker_failure();
            server_.poll_once(-1);
        }
        rethrow_worker_failure();
    }

    void TcpGateway::poll_once(int timeout_ms) {
        rethrow_worker_failure();
        server_.poll_once(timeout_ms);
        rethrow_worker_failure();
    }

    void TcpGateway::request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        try {
            command_queue_.close_and_discard();
        } catch (...) {
        }
        try {
            response_queue_.close_and_discard();
        } catch (...) {
        }
        server_.notify();
    }

    std::uint16_t TcpGateway::local_port() const noexcept {
        return server_.local_port();
    }

    std::size_t TcpGateway::connection_count() const noexcept {
        return server_.connection_count();
    }

    void TcpGateway::handle_line(
        ConnectionId connection_id,
        std::string_view line) {
        CommandEnvelope envelope{
            connection_id,
            parse_trading_request(line)};
        if (!command_queue_.try_push(std::move(envelope))) {
            static_cast<void>(server_.request_close(connection_id));
            return;
        }

        if (!server_.mark_request_in_flight(connection_id)) {
            throw std::logic_error(
                "live connection disappeared during command submission");
        }
    }

    void TcpGateway::handle_wakeup() {
        while (std::optional<ResponseEnvelope> response =
                   response_queue_.try_pop()) {
            server_.queue_write(
                response->connection_id,
                std::move(response->encoded_response));
            static_cast<void>(
                server_.complete_request(response->connection_id));
        }
        rethrow_worker_failure();
    }

    void TcpGateway::execution_loop() noexcept {
        try {
            while (std::optional<CommandEnvelope> envelope =
                       command_queue_.wait_pop()) {
                std::string response = execute_request(
                    envelope->request,
                    runtime_->executor());

                if (!response_queue_.wait_push(ResponseEnvelope{
                        envelope->connection_id,
                        std::move(response)})) {
                    return;
                }
                server_.notify();
            }
        } catch (...) {
            worker_failure_ = std::current_exception();
            worker_failure_ready_.store(true, std::memory_order_release);
            request_stop();
        }
    }

    void TcpGateway::rethrow_worker_failure() const {
        if (worker_failure_ready_.load(std::memory_order_acquire)) {
            std::rethrow_exception(worker_failure_);
        }
    }

    std::string TcpGateway::execute_request(
        const TradingRequestParseResult& request,
        TradingRequestExecutor& executor) {
        if (const auto* error = std::get_if<ProtocolError>(&request)) {
            return encode_error(*error);
        }
        return encode_trading_response(
            executor.execute(std::get<TradingRequest>(request)));
    }
}  // namespace exchange
