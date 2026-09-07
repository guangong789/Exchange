#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "exchange/gateway/tcp_gateway.hpp"

namespace {
    std::unique_ptr<exchange::TradingRuntime> make_runtime(
        std::string_view wal_path) {
        constexpr exchange::InstrumentContext instrument{1, 2, 1, 1, 1};
        constexpr exchange::Amount initial_balance = 1'000'000'000;
        const exchange::TradingBootstrapConfig bootstrap{{
            exchange::BootstrapAccount{
                1,
                {{instrument.base_asset, {initial_balance, 0}},
                 {instrument.quote_asset, {initial_balance, 0}}}},
            exchange::BootstrapAccount{
                2,
                {{instrument.base_asset, {initial_balance, 0}},
                 {instrument.quote_asset, {initial_balance, 0}}}},
        }};

        return exchange::TradingRuntime::create_durable(
            instrument,
            std::string{wal_path},
            bootstrap);
    }
}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: exchange_server <port> <wal-path>\n";
        return 1;
    }

    const std::string_view port_text{argv[1]};
    unsigned int parsed_port = 0;
    const auto [parsed_to, parse_error] = std::from_chars(
        port_text.data(),
        port_text.data() + port_text.size(),
        parsed_port);
    if (parse_error != std::errc{} ||
        parsed_to != port_text.data() + port_text.size() ||
        parsed_port == 0 ||
        parsed_port > std::numeric_limits<std::uint16_t>::max()) {
        std::cerr << "Invalid port: " << port_text << '\n';
        return 1;
    }

    try {
        exchange::TcpGateway gateway(
            static_cast<std::uint16_t>(parsed_port),
            make_runtime(argv[2]));
        std::cout << "exchange_server listening on 127.0.0.1:"
                  << gateway.local_port() << std::endl;
        gateway.run();
    } catch (const std::exception& error) {
        std::cerr << "exchange_server error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
