#include "exchange/replay/replay_engine.hpp"

#include <type_traits>
#include <variant>

namespace exchange {
    ReplayEngine::ReplayEngine(MatchingEngine& matching_engine) noexcept
        : matching_engine_(matching_engine) {}

    void ReplayEngine::replay(const std::vector<Command>& commands) {
        for (const Command& command : commands) {
            std::visit(
                [this](const auto& payload) {
                    using Payload = std::decay_t<decltype(payload)>;

                    if constexpr (std::is_same_v<Payload, AddOrder>) {
                        static_cast<void>(matching_engine_.add_order(payload.order));
                    } else if constexpr (std::is_same_v<Payload, CancelOrder>) {
                        static_cast<void>(
                            matching_engine_.cancel_order(payload.order_id));
                    }
                },
                command.payload);
        }
    }
    // 解包 std::variant，将当前实际类型传给 lambda。
    // 再通过 if constexpr 在编译期区分 AddOrder / CancelOrder，
    // 调用对应的 MatchingEngine 接口。
}
