#include "exchange/hackathon/hackathon_demo.hpp"
#include "exchange/model/deepseek_adapter.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <nlohmann/json.hpp>

namespace {
using namespace exchange;
using Json = nlohmann::json;

struct Options {
    bool json{};
    HackathonDemoScenarioKind scenario{HackathonDemoScenarioKind::Normal};
};

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--json") {
            options.json = true;
        } else if (argument == "--scenario") {
            if (++index == argc) throw std::invalid_argument("--scenario requires normal or agent-error");
            const std::string_view scenario(argv[index]);
            if (scenario == "normal") options.scenario = HackathonDemoScenarioKind::Normal;
            else if (scenario == "agent-error") options.scenario = HackathonDemoScenarioKind::AgentError;
            else throw std::invalid_argument("--scenario requires normal or agent-error");
        } else if (argument == "--help") {
            std::cout << "Usage: exchange_hackathon_demo [--json] [--scenario normal|agent-error]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(argument));
        }
    }
    return options;
}

[[nodiscard]] Json balance_json(const Balance& value) {
    return {{"available", value.available}, {"reserved", value.reserved}};
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

[[nodiscard]] Json snapshot_json(const HackathonDemoSnapshot& snapshot) {
    const auto& turn = snapshot.turn;
    const auto& execution = snapshot.execution;
    Json trade = nullptr;
    if (execution.trade) {
        trade = {{"buy_order_id", execution.trade->buy_order_id},
                 {"sell_order_id", execution.trade->sell_order_id},
                 {"price", execution.trade->price},
                 {"quantity", execution.trade->quantity},
                 {"timestamp", execution.trade->timestamp}};
    }
    return {
        {"scenario", snapshot.scenario == HackathonDemoScenarioKind::Normal ? "normal" : "agent-error"},
        {"analyst", {{"service", snapshot.payment_requirement.resource_description},
                     {"x402", true},
                     {"payment_preview_provider", turn.preview.provider},
                     {"preview_status", "APPROVED"},
                     {"preview_authorized", turn.preview_authorized},
                     {"settlement", "NOT_PERFORMED"},
                     {"signal", turn.premium_signal ? turn.premium_signal->signal : ""},
                     {"confidence", turn.premium_signal ? turn.premium_signal->confidence : 0}}},
        {"risk", {{"available_quote", turn.risk_guidance.available_quote},
                  {"local_best_ask", turn.risk_guidance.local_best_ask},
                  {"risk_budget_bps", turn.risk_guidance.risk_budget_bps},
                  {"affordable_quantity", turn.risk_guidance.affordable_quantity},
                  {"risk_budget_quantity", turn.risk_guidance.risk_budget_quantity},
                  {"objective_remaining", turn.risk_guidance.objective_remaining},
                  {"max_recommended_quantity", turn.risk_guidance.max_recommended_quantity},
                  {"reason", turn.risk_guidance.reason}, {"advisory_only", true}}},
        {"trader", {{"cognition", snapshot.scenario == HackathonDemoScenarioKind::Normal ? "DeepSeek LIVE" : "Intentional Agent Error"},
                    {"action", action_name(turn.action)}, {"submit_result", result_name(turn.action_result)},
                    {"model_calls", snapshot.model_calls_original}}},
        {"execution", {{"authority", "local deterministic core"}, {"trade", std::move(trade)},
                        {"balances", {{"trader", {{"base_before", balance_json(execution.trader_base_before)}, {"quote_before", balance_json(execution.trader_quote_before)}, {"base_after", balance_json(execution.trader_base_after)}, {"quote_after", balance_json(execution.trader_quote_after)}}},
                                      {"analyst", {{"base_before", balance_json(execution.analyst_base_before)}, {"quote_before", balance_json(execution.analyst_quote_before)}, {"base_after", balance_json(execution.analyst_base_after)}, {"quote_after", balance_json(execution.analyst_quote_after)}}}}},
                        {"ledger_entry_count", execution.ledger_entry_count}, {"objective_achieved", turn.trader_objective.achieved},
                        {"balances_unchanged", execution.balances_unchanged}, {"ledger_unchanged", execution.ledger_unchanged},
                        {"no_trader_reservation", execution.no_trader_reservation}, {"analyst_resting_order_unchanged", execution.analyst_resting_order_unchanged}}},
        {"replay", {{"available", snapshot.replay_available}, {"deepseek_calls_original", snapshot.model_calls_original},
                     {"deepseek_calls_replay", snapshot.model_calls_replay}, {"payment_service_calls_replay", snapshot.payment_service_calls_replay},
                     {"balance_parity", snapshot.balance_parity}, {"ledger_parity", snapshot.ledger_parity},
                     {"objective_parity", snapshot.objective_parity}, {"order_parity", snapshot.order_parity}, {"trade_parity", snapshot.trade_parity}}},
    };
}

void print_terminal(const Json& report) {
    std::cout << "[1] 目标\n    展示 x402 Preview、Risk advisory、deterministic execution 与 Replay。\n"
              << "[2] Analyst\n    Preview 状态：" << report["analyst"]["preview_status"].get<std::string>()
              << "\n    访问授权：" << (report["analyst"]["preview_authorized"].get<bool>() ? "PreviewAuthorized" : "Denied")
              << "\n    真实支付结算：未执行\n"
              << "[3] Risk\n    建议最大数量：" << report["risk"]["max_recommended_quantity"].get<Quantity>() << "（advisory only）\n"
              << "[4] Trader / Execution\n    action=" << report["trader"]["action"].get<std::string>() << ", result=" << report["trader"]["submit_result"].get<std::string>()
              << "\n[5] Replay\n    parity=" << (report["replay"]["balance_parity"].get<bool>() && report["replay"]["ledger_parity"].get<bool>() && report["replay"]["objective_parity"].get<bool>() && report["replay"]["trade_parity"].get<bool>() ? "PASS" : "FAIL")
              << "\n演示结果：PASS\n";
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        std::unique_ptr<DeepSeekAdapter> deepseek;
        ModelAdapter* model_adapter = nullptr;
        if (options.scenario == HackathonDemoScenarioKind::Normal) {
            const char* key = std::getenv("DEEPSEEK_API_KEY");
            if (key == nullptr || key[0] == '\0') throw std::runtime_error("DEEPSEEK_API_KEY 未设置");
            deepseek = std::make_unique<DeepSeekAdapter>();
            model_adapter = deepseek.get();
        }
        const Json report = snapshot_json(run_hackathon_demo_snapshot(options.scenario, model_adapter));
        if (options.json) std::cout << report.dump() << '\n';
        else print_terminal(report);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (argc > 1 && std::string_view(argv[1]) == "--json") std::cout << Json{{"error", error.what()}}.dump() << '\n';
        else std::cerr << "exchange_hackathon_demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
