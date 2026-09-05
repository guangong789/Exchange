#include "exchange/hackathon/hackathon_demo.hpp"
#include "exchange/hackathon/live_x402_evidence.hpp"
#include "exchange/model/deepseek_adapter.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include <nlohmann/json.hpp>

namespace {
using namespace exchange;
using Json = nlohmann::json;

[[nodiscard]] std::string simulation_status_name(HackathonSimulationStatus status) {
    switch (status) {
        case HackathonSimulationStatus::Idle: return "IDLE";
        case HackathonSimulationStatus::Running: return "RUNNING";
        case HackathonSimulationStatus::StopRequested: return "STOP_REQUESTED";
        case HackathonSimulationStatus::GoalAchieved: return "GOAL_ACHIEVED";
        case HackathonSimulationStatus::UserStopped: return "USER_STOPPED";
        case HackathonSimulationStatus::MaxRounds: return "MAX_ROUNDS";
        case HackathonSimulationStatus::Error: return "ERROR";
    }
    return "ERROR";
}

[[nodiscard]] std::string replay_status_name(HackathonReplayStatus status) {
    switch (status) {
        case HackathonReplayStatus::NotRun: return "NOT_RUN";
        case HackathonReplayStatus::Running: return "RUNNING";
        case HackathonReplayStatus::Exact: return "EXACT";
        case HackathonReplayStatus::Mismatch: return "MISMATCH";
    }
    return "MISMATCH";
}

[[nodiscard]] std::string action_name(const AgentAction& action) {
    return std::visit([](const auto& value) -> std::string {
        using Action = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Action, SubmitOrderAction>) {
            return std::string("SUBMIT ") + (value.side == Side::Buy ? "BUY" : "SELL")
                + " " + std::to_string(value.quantity) + " @ " + std::to_string(value.price);
        } else if constexpr (std::is_same_v<Action, CancelOrderAction>) {
            return "CANCEL " + std::to_string(value.order_id);
        }
        return "HOLD";
    }, action);
}

[[nodiscard]] std::string result_name(const AgentActionResult& result) {
    return std::visit([](const auto& value) -> std::string {
        using Result = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Result, SubmitActionResult>) {
            switch (value.result) {
                case SubmitResult::Accepted: return "Accepted";
                case SubmitResult::InsufficientFunds: return "InsufficientFunds";
                case SubmitResult::DuplicateOrder: return "DuplicateOrder";
                case SubmitResult::InvalidOrder: return "InvalidOrder";
                case SubmitResult::CounterpartyNotAccountBacked: return "CounterpartyNotAccountBacked";
            }
        } else if constexpr (std::is_same_v<Result, CancelActionResult>) {
            switch (value.result) {
                case CancelResult::Cancelled: return "Cancelled";
                case CancelResult::NotFound: return "NotFound";
                case CancelResult::NotOwner: return "NotOwner";
            }
        }
        return "Held";
    }, result);
}

[[nodiscard]] Json balance_json(const Balance& balance) {
    return {{"available", balance.available}, {"reserved", balance.reserved}};
}

[[nodiscard]] Json order_json(const Order& order) {
    return {{"id", order.id}, {"side", order.side == Side::Buy ? "BUY" : "SELL"},
            {"price", order.price}, {"quantity", order.quantity}, {"timestamp", order.timestamp}};
}

[[nodiscard]] Json trade_json(const Trade& trade) {
    return {{"buy_order_id", trade.buy_order_id}, {"sell_order_id", trade.sell_order_id},
            {"price", trade.price}, {"quantity", trade.quantity}, {"timestamp", trade.timestamp}};
}

[[nodiscard]] Json score_json(const HackathonEpisodeScore& score) {
    return {{"total", score.total},
            {"objective_progress", score.objective_progress_points},
            {"accepted_actions", score.accepted_action_points},
            {"risk_compliant", score.risk_compliant_points},
            {"useful_holds", score.useful_hold_points},
            {"rejected_actions", score.rejected_action_points},
            {"risk_violations", score.risk_violation_points}};
}

[[nodiscard]] std::string live_status_name(LiveX402EvidenceStatus status) {
    switch (status) {
        case LiveX402EvidenceStatus::NotRun: return "NOT_RUN";
        case LiveX402EvidenceStatus::Complete: return "COMPLETE";
        case LiveX402EvidenceStatus::Error: return "ERROR";
    }
    return "ERROR";
}

[[nodiscard]] Json live_x402_json(const LiveX402Evidence& value) {
    const auto& requirement = value.requirement.requirement;
    return {{"status", live_status_name(value.status)},
            {"requirement", {{"x402_version", requirement.x402_version},
                             {"resource_url", requirement.resource_url},
                             {"description", requirement.resource_description},
                             {"mime_type", requirement.resource_mime_type},
                             {"scheme", requirement.scheme}, {"network", requirement.network},
                             {"asset", requirement.asset}, {"amount", requirement.amount},
                             {"pay_to", requirement.pay_to},
                             {"max_timeout_seconds", requirement.max_timeout_seconds},
                             {"extra", {{"name", value.requirement.accept_extra.name},
                                        {"version", value.requirement.accept_extra.version}}}}},
            {"wallet_status", value.wallet_status}, {"provider", value.provider},
            {"provider_status", value.provider_status}, {"reasons", value.reasons},
            {"preview_performed", value.preview_performed},
            {"payment_signable", value.payment_signable}, {"signed", value.payment_signed},
            {"broadcast", value.broadcast}, {"settlement_performed", value.settlement_performed},
            {"service_unlocked", value.service_unlocked}, {"funds_moved", 0},
            {"error_code", value.error_code}, {"error_message", value.error_message},
            {"started_at_unix_ms", value.started_at_unix_ms},
            {"completed_at_unix_ms", value.completed_at_unix_ms},
            {"duration_ms", value.duration_ms}};
}

[[nodiscard]] Json replay_world_state_json(
    const HackathonReplayWorldState& state) {
    return {
        {"trader_base", balance_json(state.trader_base)},
        {"trader_quote", balance_json(state.trader_quote)},
        {"trade_records", state.trade_records},
        {"filled_base_quantity", state.filled_base_quantity},
        {"executed_quote_amount", state.executed_quote_amount},
        {"active_orders", state.active_orders},
        {"ledger_entries", state.ledger_entries},
        {"objective", {{"current", state.objective.current_amount},
                       {"target", state.objective.target_amount},
                       {"achieved", state.objective.achieved}}},
    };
}

[[nodiscard]] Json snapshot_json(const HackathonSimulationSnapshot& snapshot) {
    Json activities = Json::array();
    for (const auto& activity : snapshot.activities) {
        activities.push_back({{"sequence", activity.sequence}, {"round", activity.round},
                              {"role", activity.role}, {"detail", activity.detail}});
    }
    Json stages = Json::array();
    for (const auto& stage : snapshot.replay.stages) {
        stages.push_back({{"name", stage.name}, {"state", stage.state}});
    }
    Json match_trades = Json::array();
    for (const Trade& trade : snapshot.match_engine.trades) {
        match_trades.push_back(trade_json(trade));
    }
    Json round_evidence = Json::array();
    for (const auto& round : snapshot.round_evidence) {
        Json trades = Json::array();
        for (const Trade& trade : round.trades) trades.push_back(trade_json(trade));
        round_evidence.push_back({
            {"round", round.round},
            {"external_market", {{"symbol", round.external_market.symbol}, {"best_bid", round.external_market.best_bid}, {"best_ask", round.external_market.best_ask}}},
            {"analyst", {{"signal", round.analyst_signal.signal}, {"confidence", round.analyst_signal.confidence}, {"reason", round.analyst_public_reason}}},
            {"risk", {{"available_quote", round.risk_guidance.available_quote}, {"local_best_ask", round.risk_guidance.local_best_ask}, {"risk_budget_bps", round.risk_guidance.risk_budget_bps}, {"affordable_quantity", round.risk_guidance.affordable_quantity}, {"risk_budget_quantity", round.risk_guidance.risk_budget_quantity}, {"objective_remaining", round.risk_guidance.objective_remaining}, {"max_recommended_quantity", round.risk_guidance.max_recommended_quantity}}},
            {"action", action_name(round.action)}, {"submit_result", result_name(round.action_result)},
            {"trades", std::move(trades)},
            {"resting_order", round.resting_order.has_value() ? Json(order_json(*round.resting_order)) : Json(nullptr)},
            {"reserved_quote_after", round.reserved_quote_after},
            {"objective", {{"current", round.objective_after.current_amount}, {"target", round.objective_after.target_amount}, {"achieved", round.objective_after.achieved}}},
            {"score", {{"before", round.score_before}, {"delta", round.score_delta}, {"after", round.score_after}, {"reasons", round.score_reasons}}},
        });
    }
    Json last_turn = nullptr;
    if (snapshot.last_turn.has_value()) {
        const auto& turn = *snapshot.last_turn;
        last_turn = {
            {"preview_authorized", turn.preview_authorized},
            {"preview_status", turn.preview.status == ExternalPaymentPreviewStatus::Approved ? "APPROVED" : "REJECTED"},
            {"settlement", "NOT_PERFORMED"},
            {"signal", turn.premium_signal.has_value() ? turn.premium_signal->signal : ""},
            {"confidence", turn.premium_signal.has_value() ? turn.premium_signal->confidence : 0},
            {"risk", {{"available_quote", turn.risk_guidance.available_quote}, {"local_best_ask", turn.risk_guidance.local_best_ask},
                      {"risk_budget_bps", turn.risk_guidance.risk_budget_bps}, {"affordable_quantity", turn.risk_guidance.affordable_quantity},
                      {"risk_budget_quantity", turn.risk_guidance.risk_budget_quantity}, {"objective_remaining", turn.risk_guidance.objective_remaining},
                      {"max_recommended_quantity", turn.risk_guidance.max_recommended_quantity}}},
            {"action", action_name(turn.action)}, {"submit_result", result_name(turn.action_result)},
        };
    }
    Json summary = nullptr;
    if (snapshot.summary.has_value()) {
        const auto& value = *snapshot.summary;
        summary = {{"end_reason", value.end_reason}, {"seed", value.seed}, {"rounds_completed", value.rounds_completed},
                   {"deepseek_calls", value.deepseek_calls}, {"analyst_service_accesses", value.analyst_service_accesses},
                   {"trader_decisions", value.trader_decisions}, {"orders_submitted", value.orders_submitted}, {"accepted_actions", value.accepted_actions},
                   {"rejected_actions", value.rejected_actions}, {"held_actions", value.held_actions},
                   {"trades", value.trades}, {"base_acquired", value.base_acquired}, {"quote_spent", value.quote_spent},
                   {"current_reserved_quote", value.current_reserved_quote}, {"filled_base_quantity", value.filled_base_quantity},
                   {"final_base", value.final_base}, {"objective_target", value.objective_target},
                   {"ledger_entries", value.ledger_entries}, {"active_orders", value.active_orders},
                   {"invalid_state_mutations", value.invalid_state_mutations},
                   {"score", score_json(value.score)}};
    }
    return {
        {"scenario", snapshot.scenario == HackathonDemoScenarioKind::Normal ? "normal" : "agent-error"},
        {"status", simulation_status_name(snapshot.status)},
        {"round", {{"current", snapshot.rounds_completed}, {"max", snapshot.config.max_rounds}}},
        {"seed", snapshot.config.seed}, {"score", score_json(snapshot.score)},
        {"objective", {{"current", snapshot.objective.current_amount}, {"target", snapshot.objective.target_amount}, {"achieved", snapshot.objective.achieved}}},
        {"runtime", {{"trader_base", balance_json(snapshot.trader_base)}, {"trader_quote", balance_json(snapshot.trader_quote)},
                     {"analyst_base", balance_json(snapshot.analyst_base)}, {"analyst_quote", balance_json(snapshot.analyst_quote)},
                     {"active_orders", snapshot.active_orders}, {"trades", snapshot.trade_count}, {"rejected_actions", snapshot.rejected_actions}, {"ledger_entries", snapshot.ledger_entries}}},
        {"last_turn", std::move(last_turn)},
        {"match_engine", {{"incoming_order", snapshot.match_engine.incoming_order.has_value() ? Json(order_json(*snapshot.match_engine.incoming_order)) : Json(nullptr)},
                          {"best_bid_before", snapshot.match_engine.best_bid_before.has_value() ? Json(*snapshot.match_engine.best_bid_before) : Json(nullptr)},
                          {"best_ask_before", snapshot.match_engine.best_ask_before.has_value() ? Json(*snapshot.match_engine.best_ask_before) : Json(nullptr)},
                          {"price_time_priority", snapshot.match_engine.price_time_priority},
                          {"trade", snapshot.match_engine.trade.has_value() ? Json(trade_json(*snapshot.match_engine.trade)) : Json(nullptr)},
                          {"trades", std::move(match_trades)},
                          {"maker_remaining_quantity", snapshot.match_engine.maker_remaining_quantity}, {"active_orders", snapshot.match_engine.active_order_count},
                          {"maker_orders_consumed", snapshot.match_engine.maker_orders_consumed}, {"multi_level_taker", snapshot.match_engine.multi_level_taker}, {"partial_fill", snapshot.match_engine.partial_fill},
                          {"reservation_consumed", snapshot.match_engine.reservation_consumed},
                          {"balances_unchanged", snapshot.match_engine.balances_unchanged},
                          {"ledger_unchanged", snapshot.match_engine.ledger_unchanged},
                          {"no_residual_reservation", snapshot.match_engine.no_residual_reservation}}},
        {"activities", std::move(activities)}, {"round_evidence", std::move(round_evidence)}, {"summary", std::move(summary)},
        {"replay", {{"status", replay_status_name(snapshot.replay.status)}, {"stages", std::move(stages)},
                    {"deepseek_calls_original", snapshot.replay.deepseek_calls_original}, {"deepseek_calls_replay", snapshot.replay.deepseek_calls_replay},
                    {"payment_service_calls_original", snapshot.replay.payment_service_calls_original}, {"payment_service_calls_replay", snapshot.replay.payment_service_calls_replay},
                    {"captured_economic_inputs", snapshot.replay.captured_economic_inputs},
                    {"original_final_state", snapshot.replay.original_final_state.has_value() ? Json(replay_world_state_json(*snapshot.replay.original_final_state)) : Json(nullptr)},
                    {"replay_final_state", snapshot.replay.replay_final_state.has_value() ? Json(replay_world_state_json(*snapshot.replay.replay_final_state)) : Json(nullptr)},
                    {"balance_parity", snapshot.replay.balance_parity}, {"ledger_parity", snapshot.replay.ledger_parity},
                    {"objective_parity", snapshot.replay.objective_parity}, {"order_parity", snapshot.replay.order_parity}, {"trade_parity", snapshot.replay.trade_parity}}},
    };
}

class BridgeSession {
public:
    [[nodiscard]] Json handle(const Json& request) {
        const std::string action = request.at("action").get<std::string>();
        if (action == "live-x402-snapshot") {
            return {{"ok", true}, {"live_x402", live_x402_json(live_x402_)}};
        }
        if (action == "live-x402-check") {
            const char* configured_cli = std::getenv("EXCHANGE_BINANCE_WALLET_CLI");
            live_x402_ = LiveX402EvidenceRunner({
                configured_cli == nullptr ? "baw" : configured_cli,
                std::chrono::seconds(30)}).run();
            return {{"ok", true}, {"live_x402", live_x402_json(live_x402_)}};
        }
        if (action == "start") {
            const std::string scenario = request.value("scenario", "normal");
            const auto kind = scenario == "normal" ? HackathonDemoScenarioKind::Normal
                : scenario == "agent-error" ? HackathonDemoScenarioKind::AgentError
                : throw std::invalid_argument("unsupported scenario");
            model_.reset();
            if (kind == HackathonDemoScenarioKind::Normal) {
                const char* key = std::getenv("DEEPSEEK_API_KEY");
                if (key == nullptr || key[0] == '\0') throw std::runtime_error("DEEPSEEK_API_KEY 未设置");
                model_ = std::make_unique<DeepSeekAdapter>();
            }
            simulation_ = std::make_unique<HackathonSimulation>(kind, model_.get());
            simulation_->start();
        } else if (action == "snapshot") {
            require_simulation();
        } else if (action == "advance") {
            require_simulation(); simulation_->advance();
        } else if (action == "stop") {
            require_simulation(); simulation_->request_stop();
        } else if (action == "replay-start") {
            require_simulation(); simulation_->start_replay();
        } else if (action == "replay-advance") {
            require_simulation(); simulation_->advance_replay();
        } else {
            throw std::invalid_argument("unsupported bridge action");
        }
        return {{"ok", true}, {"snapshot", snapshot_json(simulation_->snapshot())}};
    }

private:
    void require_simulation() const {
        if (!simulation_) throw std::logic_error("simulation has not started");
    }

    std::unique_ptr<DeepSeekAdapter> model_;
    std::unique_ptr<HackathonSimulation> simulation_;
    LiveX402Evidence live_x402_;
};
}  // namespace

int main() {
    BridgeSession session;
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const Json request = Json::parse(line);
            if (request.value("action", "") == "shutdown") {
                std::cout << Json{{"ok", true}}.dump() << std::endl;
                break;
            }
            std::cout << session.handle(request).dump() << std::endl;
        } catch (const std::exception& error) {
            std::cout << Json{{"ok", false}, {"error", error.what()}}.dump() << std::endl;
        }
    }
    return EXIT_SUCCESS;
}
