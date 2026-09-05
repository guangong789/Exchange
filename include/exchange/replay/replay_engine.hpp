#pragma once

#include <vector>

#include "exchange/matching/command.hpp"
#include "exchange/matching/matching_engine.hpp"

namespace exchange {
    class ReplayEngine {
    public:
        explicit ReplayEngine(MatchingEngine& matching_engine) noexcept;

        void replay(const std::vector<Command>& commands);

    private:
        MatchingEngine& matching_engine_;
    };
}
