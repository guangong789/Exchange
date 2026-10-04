#include "diagnostics/partial_fill_evidence.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        const auto evidence =
            exchange::diagnostics::make_partial_fill_evidence();
        std::cout
            << exchange::diagnostics::partial_fill_evidence_json(evidence)
            << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exchange_diagnostic_evidence: "
                  << error.what() << '\n';
        return 1;
    }
}
