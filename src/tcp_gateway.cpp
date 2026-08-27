#include "exchange/tcp_gateway.hpp"

#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "exchange/line_protocol.hpp"

namespace exchange {
    TcpGateway::TcpGateway(
        std::uint16_t port,
        std::size_t command_queue_capacity,
        std::size_t response_queue_capacity)
        : command_queue_(command_queue_capacity),
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
        matching_thread_ = std::thread(&TcpGateway::matching_loop, this);
    }

    TcpGateway::~TcpGateway() {
        request_stop();
        if (matching_thread_.joinable()) {
            matching_thread_.join();
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
            parse_command(line)};
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

    void TcpGateway::matching_loop() noexcept {
        try {
            EventCollector event_collector;
            MatchingEngine matching_engine{event_collector};

            while (std::optional<CommandEnvelope> envelope =
                       command_queue_.wait_pop()) {
                event_collector.clear();

                std::string response;
                try {
                    response = execute_request(
                        envelope->request,
                        matching_engine,
                        event_collector);
                } catch (...) {
                    event_collector.clear();
                    throw;
                }

                event_collector.clear();
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
        const CommandParseResult& request,
        MatchingEngine& matching_engine,
        EventCollector& event_collector) {
        if (const auto* error = std::get_if<ProtocolError>(&request)) {
            return encode_error(*error);
        }
        return execute_command(
            std::get<Command>(request),
            matching_engine,
            event_collector);
    }

    std::string TcpGateway::execute_command(
        const Command& command,
        MatchingEngine& matching_engine,
        EventCollector& event_collector) {
        return std::visit(
            [&matching_engine,
             &event_collector](const auto& payload) -> std::string {
                using Payload = std::decay_t<decltype(payload)>;

                if constexpr (std::is_same_v<Payload, AddOrder>) {
                    try {
                        static_cast<void>(
                            matching_engine.add_order(payload.order));
                    } catch (const std::invalid_argument&) {
                        return encode_error(ProtocolError{
                            ProtocolErrorCode::InvalidOrder, 0});
                    }
                } else if constexpr (std::is_same_v<Payload, CancelOrder>) {
                    if (!matching_engine.cancel_order(payload.order_id)) {
                        return encode_error(ProtocolError{
                            ProtocolErrorCode::CancelNotFound,
                            payload.order_id});
                    }
                }

                const auto& events = event_collector.events();
                return encode_success(std::span<const Event>{events});
            },
            command.payload);
    }
}  // namespace exchange
