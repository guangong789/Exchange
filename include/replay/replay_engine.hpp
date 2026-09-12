#pragma once

#include <vector>

#include "matching/command.hpp"
#include "matching/matching_engine.hpp"

namespace exchange {
    class ReplayEngine {
    public:
        explicit ReplayEngine(MatchingEngine& matching_engine) noexcept;

        void replay(const std::vector<Command>& commands);

    private:
        MatchingEngine& matching_engine_;
    };
}
