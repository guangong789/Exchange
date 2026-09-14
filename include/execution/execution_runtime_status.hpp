#pragma once

namespace exchange {
    struct ExecutionRuntimeStatus {
        bool poisoned{};
        bool durable_processing_started{};
    };
}  // namespace exchange
