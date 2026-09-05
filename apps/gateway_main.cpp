#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

#include "exchange/gateway/tcp_gateway.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: exchange_server <port>\n";
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
            static_cast<std::uint16_t>(parsed_port));
        std::cout << "exchange_server listening on 127.0.0.1:"
                  << gateway.local_port() << std::endl;
        gateway.run();
    } catch (const std::system_error& error) {
        std::cerr << "exchange_server error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
