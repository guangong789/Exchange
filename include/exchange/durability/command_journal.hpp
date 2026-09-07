#pragma once

#include "exchange/execution/execution_command.hpp"

namespace exchange {
    class ExecutionCommandJournal {
    public:
        virtual ~ExecutionCommandJournal() = default;

        // Success means the complete command is durably admitted.
        virtual void append(const ExecutionCommand& command) = 0;
    };
}  // namespace exchange
