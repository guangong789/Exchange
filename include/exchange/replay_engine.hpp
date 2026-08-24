#pragma once

#include <vector>

#include "exchange/command.hpp"
#include "exchange/matching_engine.hpp"

namespace exchange {
    class ReplayEngine {
    public:
        explicit ReplayEngine(MatchingEngine& matching_engine) noexcept;

        void replay(const std::vector<Command>& commands);

    private:
        MatchingEngine& matching_engine_;
    };
}
