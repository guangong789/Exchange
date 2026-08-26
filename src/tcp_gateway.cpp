#include "exchange/tcp_gateway.hpp"

#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "exchange/line_protocol.hpp"

namespace exchange {
    TcpGateway::TcpGateway(
        std::uint16_t port)
        : matching_engine_(event_collector_),
          server_(
              port,
              [this](int client_fd, std::string_view line) {
                  handle_line(client_fd, line);
              }) {}

    void TcpGateway::run() {
        server_.run();
    }

    void TcpGateway::poll_once(int timeout_ms) {
        server_.poll_once(timeout_ms);
    }

    std::uint16_t TcpGateway::local_port() const noexcept {
        return server_.local_port();
    }

    std::size_t TcpGateway::connection_count() const noexcept {
        return server_.connection_count();
    }

    void TcpGateway::handle_line(int client_fd, std::string_view line) {
        event_collector_.clear();

        std::string response;
        try {
            CommandParseResult parsed = parse_command(line);
            if (const auto* error = std::get_if<ProtocolError>(&parsed)) {
                response = encode_error(*error);
            } else {
                response = execute_command(std::get<Command>(parsed));
            }
        } catch (...) {
            event_collector_.clear();
            throw;
        }

        event_collector_.clear();
        server_.queue_write(client_fd, std::move(response));
    }

    std::string TcpGateway::execute_command(const Command& command) {
        return std::visit(
            [this](const auto& payload) -> std::string {
                using Payload = std::decay_t<decltype(payload)>;

                if constexpr (std::is_same_v<Payload, AddOrder>) {
                    try {
                        static_cast<void>(
                            matching_engine_.add_order(payload.order));
                    } catch (const std::invalid_argument&) {
                        return encode_error(ProtocolError{
                            ProtocolErrorCode::InvalidOrder, 0});
                    }
                } else if constexpr (std::is_same_v<Payload, CancelOrder>) {
                    if (!matching_engine_.cancel_order(payload.order_id)) {
                        return encode_error(ProtocolError{
                            ProtocolErrorCode::CancelNotFound,
                            payload.order_id});
                    }
                }

                const auto& events = event_collector_.events();
                return encode_success(std::span<const Event>{events});
            },
            command.payload);
    }
}  // namespace exchange
