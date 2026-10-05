#include "diagnostics/historical_order_evidence.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    std::string wal_path;
    std::string config_path;
    exchange::OrderId order_id = 0;
    bool valid = argc == 7;
    for (int index = 1; valid && index < argc; index += 2) {
        const std::string_view option{argv[index]};
        const std::string_view value{argv[index + 1]};
        if (option == "--wal" && wal_path.empty() && !value.empty()) {
            wal_path = value;
        } else if (option == "--config" && config_path.empty() && !value.empty()) {
            config_path = value;
        } else if (option == "--order-id" && order_id == 0) {
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), order_id);
            valid = error == std::errc{} && end == value.data() + value.size()
                && order_id != 0;
        } else {
            valid = false;
        }
    }
    if (!valid || wal_path.empty() || config_path.empty() || order_id == 0) {
        std::cerr << "Usage: exchange_historical_order_evidence "
                     "--wal <offline-wal> --order-id <positive-id> --config <json>\n"
                     "Source WAL must be offline; recovery operates on a private copy.\n";
        return 1;
    }
    try {
        const auto config = exchange::diagnostics::load_historical_evidence_config(config_path);
        const auto evidence = exchange::diagnostics::reconstruct_historical_order_evidence(
            config, wal_path, order_id);
        if (!evidence) {
            std::cout << "{\"error\":\"order_not_found\",\"order_id\":" << order_id << "}\n";
            return 2;
        }
        std::cout << exchange::diagnostics::historical_order_evidence_json(*evidence) << '\n';
        return 0;
    } catch (const std::exception&) {
        // Keep stdout exclusively structured; no internal exception detail.
        std::cerr << "Historical evidence unavailable: check offline WAL and config.\n";
        return 1;
    }
}
