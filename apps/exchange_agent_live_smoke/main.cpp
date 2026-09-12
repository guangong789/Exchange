#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "agent/domain/agent_registry.hpp"
#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/provider/binance_alpha/binance_alpha_market_feed.hpp"
#include "agent/provider/deepseek/deepseek_client.hpp"
#include "agent/provider/deepseek/deepseek_decision_provider.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/execution_command.hpp"
#include "execution/trading_bootstrap.hpp"
#include "execution/trading_request.hpp"
#include "execution/trading_runtime.hpp"

namespace {
    using namespace std::chrono_literals;

    constexpr exchange::InstrumentContext instrument{20, 10, 1, 1, 1};
    constexpr exchange::AgentId agent_id = 101;
    constexpr exchange::AccountId account_id = 1;
    constexpr exchange::AccountId liquidity_account_id = 2;
    constexpr std::size_t turn_count = 2;

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string pattern =
                (std::filesystem::temp_directory_path()
                 / "exchange-agent-live-smoke-XXXXXX")
                    .string();
            std::vector<char> writable(pattern.begin(), pattern.end());
            writable.push_back('\0');
            const char* created = ::mkdtemp(writable.data());
            if (created == nullptr) {
                throw std::runtime_error(
                    "Failed to create temporary smoke directory");
            }
            path_ = created;
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] std::string wal_path() const {
            return path_ + "/execution.wal";
        }

    private:
        std::string path_;
    };

    std::int64_t system_time_millis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    exchange::ExternalMarketState wait_for_fresh_market(
        const exchange::BinanceAlphaMarketFeed& feed,
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            exchange::ExternalMarketState state =
                feed.latest(system_time_millis());
            if (state.freshness
                == exchange::ExternalMarketFreshness::Fresh) {
                return state;
            }
            if (!feed.metadata().has_value() && !feed.last_error().empty()) {
                throw std::runtime_error(
                    "Binance Alpha initialization failed: "
                    + feed.last_error());
            }
            std::this_thread::sleep_for(100ms);
        }

        std::string message =
            "Timed out waiting for a fresh Binance Alpha snapshot";
        if (!feed.last_error().empty()) {
            message += ": " + feed.last_error();
        }
        throw std::runtime_error(std::move(message));
    }

    std::string optional_external_price(
        const std::optional<exchange::ExternalPrice>& price) {
        return price.has_value()
            ? exchange::format_external_price(*price)
            : "null";
    }

    std::string format_external_market(
        const std::optional<exchange::ExternalMarketState>& state) {
        if (!state.has_value()) {
            return "none";
        }
        std::ostringstream output;
        output << "symbol=" << state->symbol
               << " latest="
               << optional_external_price(state->latest_trade_price)
               << " bid=" << optional_external_price(state->best_bid)
               << " ask=" << optional_external_price(state->best_ask)
               << " freshness=";
        switch (state->freshness) {
            case exchange::ExternalMarketFreshness::Unavailable:
                output << "unavailable";
                break;
            case exchange::ExternalMarketFreshness::Fresh:
                output << "fresh";
                break;
            case exchange::ExternalMarketFreshness::Stale:
                output << "stale";
                break;
        }
        output << " event_ms=" << state->event_timestamp_ms
               << " received_ms=" << state->local_receive_timestamp_ms;
        return output.str();
    }

    std::string format_action(const std::optional<exchange::AgentAction>& action) {
        if (!action.has_value()) {
            return "none";
        }
        return std::visit(
            [](const auto& value) {
                using Action = std::decay_t<decltype(value)>;
                std::ostringstream output;
                if constexpr (std::is_same_v<
                                  Action,
                                  exchange::SubmitOrderAction>) {
                    output << "submit side="
                           << (value.side == exchange::Side::Buy
                                   ? "buy"
                                   : "sell")
                           << " price=" << value.price
                           << " quantity=" << value.quantity;
                } else if constexpr (std::is_same_v<
                                         Action,
                                         exchange::CancelOrderAction>) {
                    output << "cancel order_id=" << value.order_id;
                } else {
                    output << "hold";
                }
                return output.str();
            },
            *action);
    }

    std::string_view validation_name(
        exchange::AgentActionValidationResult result) {
        using Result = exchange::AgentActionValidationResult;
        switch (result) {
            case Result::Valid: return "valid";
            case Result::InvalidSide: return "invalid_side";
            case Result::InvalidPrice: return "invalid_price";
            case Result::InvalidQuantity: return "invalid_quantity";
            case Result::InvalidOrderId: return "invalid_order_id";
            case Result::CancelTargetNotActive:
                return "cancel_target_not_active";
            case Result::InvalidFinancialValue:
                return "invalid_financial_value";
        }
        throw std::logic_error("Unknown structural validation result");
    }

    std::string_view constraint_name(
        exchange::AgentEconomicConstraintResult result) {
        using Result = exchange::AgentEconomicConstraintResult;
        switch (result) {
            case Result::Allowed: return "allowed";
            case Result::OrderQuantityExceeded:
                return "order_quantity_exceeded";
            case Result::OrderNotionalExceeded:
                return "order_notional_exceeded";
            case Result::BasePositionExceeded:
                return "base_position_exceeded";
            case Result::BuyPriceExceeded: return "buy_price_exceeded";
            case Result::SellPriceBelowMinimum:
                return "sell_price_below_minimum";
        }
        throw std::logic_error("Unknown economic constraint result");
    }

    std::string_view submit_status_name(exchange::AgentSubmitStatus status) {
        using Status = exchange::AgentSubmitStatus;
        switch (status) {
            case Status::Accepted: return "accepted";
            case Status::AccountNotFound: return "account_not_found";
            case Status::InsufficientFunds: return "insufficient_funds";
            case Status::DuplicateOrder: return "duplicate_order";
            case Status::InvalidOrder: return "invalid_order";
            case Status::CounterpartyNotAccountBacked:
                return "counterparty_not_account_backed";
        }
        throw std::logic_error("Unknown submit result");
    }

    std::string_view cancel_status_name(exchange::AgentCancelStatus status) {
        using Status = exchange::AgentCancelStatus;
        switch (status) {
            case Status::Cancelled: return "cancelled";
            case Status::AccountNotFound: return "account_not_found";
            case Status::NotFound: return "not_found";
            case Status::NotOwner: return "not_owner";
        }
        throw std::logic_error("Unknown cancel result");
    }

    std::string format_execution(
        const std::optional<exchange::AgentActionResult>& result) {
        if (!result.has_value()) {
            return "none";
        }
        return std::visit(
            [](const auto& value) {
                using Result = std::decay_t<decltype(value)>;
                std::ostringstream output;
                if constexpr (std::is_same_v<
                                  Result,
                                  exchange::SubmitActionResult>) {
                    output << "submit status="
                           << submit_status_name(value.status);
                    if (value.status == exchange::AgentSubmitStatus::Accepted) {
                        output << " order_id=" << value.order_id
                               << " timestamp=" << value.timestamp;
                    }
                } else if constexpr (std::is_same_v<
                                         Result,
                                         exchange::CancelActionResult>) {
                    output << "cancel status="
                           << cancel_status_name(value.status);
                } else {
                    output << "hold";
                }
                return output.str();
            },
            *result);
    }

    std::string_view outcome_name(exchange::AgentTurnStatus status) {
        using Status = exchange::AgentTurnStatus;
        switch (status) {
            case Status::DecisionFailed: return "decision_failed";
            case Status::StructuralValidationRejected:
                return "structural_validation_rejected";
            case Status::EconomicConstraintRejected:
                return "economic_constraint_rejected";
            case Status::ExecutionRejected: return "execution_rejected";
            case Status::Executed: return "executed";
            case Status::Held: return "held";
        }
        throw std::logic_error("Unknown Agent turn outcome");
    }

    std::string format_balance(const std::optional<exchange::Balance>& balance) {
        if (!balance.has_value()) {
            return "null";
        }
        return "available=" + std::to_string(balance->available)
            + ",reserved=" + std::to_string(balance->reserved);
    }

    void print_turn(
        const exchange::AgentTurnRecord& turn,
        std::optional<std::chrono::milliseconds> deepseek_latency) {
        std::cout << "turn step=" << turn.step
                  << " agent_id=" << turn.agent_id << '\n'
                  << "  external: "
                  << format_external_market(turn.external_market_context)
                  << '\n'
                  << "  action: " << format_action(turn.action) << '\n'
                  << "  structural: "
                  << (turn.validation.has_value()
                          ? validation_name(*turn.validation)
                          : "not_reached")
                  << '\n'
                  << "  economic: "
                  << (turn.economic_constraint.has_value()
                          ? constraint_name(*turn.economic_constraint)
                          : "not_reached")
                  << '\n'
                  << "  execution: "
                  << format_execution(turn.execution_result) << '\n'
                  << "  base: pre[" << format_balance(turn.pre_state.base_balance)
                  << "] post[" << format_balance(turn.post_state.base_balance)
                  << "]\n"
                  << "  quote: pre["
                  << format_balance(turn.pre_state.quote_balance)
                  << "] post[" << format_balance(turn.post_state.quote_balance)
                  << "]\n"
                  << "  utility: before="
                  << (turn.utility_before.has_value()
                          ? std::to_string(turn.utility_before->total)
                          : "not_configured")
                  << " after="
                  << (turn.utility_after.has_value()
                          ? std::to_string(turn.utility_after->total)
                          : "not_configured")
                  << " delta="
                  << (turn.utility_delta.has_value()
                          ? std::to_string(*turn.utility_delta)
                          : "not_configured")
                  << '\n'
                  << "  outcome: " << outcome_name(turn.status) << '\n';
        if (deepseek_latency.has_value()) {
            std::cout << "  deepseek_http_latency_ms: "
                      << deepseek_latency->count() << '\n';
        }
        if (turn.status == exchange::AgentTurnStatus::DecisionFailed) {
            std::cout
                << "  provider_failure: DeepSeek transport/API/strict schema "
                   "failure\n";
        }
    }

    std::vector<std::uint8_t> read_file(const std::string& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("Failed to open smoke WAL for scanning");
        }
        const std::streamsize size = input.tellg();
        if (size < 0) {
            throw std::runtime_error("Failed to determine smoke WAL size");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (size > 0
            && !input.read(
                reinterpret_cast<char*>(bytes.data()),
                size)) {
            throw std::runtime_error("Failed to read smoke WAL");
        }
        return bytes;
    }

    exchange::TradingBootstrapConfig bootstrap_config() {
        return exchange::TradingBootstrapConfig{{exchange::BootstrapAccount{
            account_id,
            {{instrument.base_asset, {10, 0}},
             {instrument.quote_asset, {10'000, 0}}}},
            exchange::BootstrapAccount{
                liquidity_account_id,
                {{instrument.base_asset, {5, 0}}}}}};
    }

    int run_smoke() {
        exchange::DeepSeekClientConfig deepseek_config =
            exchange::deepseek_config_from_environment();
        deepseek_config.timeout = 10s;
        exchange::DeepSeekClient deepseek_client{
            std::move(deepseek_config)};
        std::vector<std::chrono::milliseconds> deepseek_latencies;
        exchange::DeepSeekDecisionProvider deepseek_provider{
            [&](const exchange::DeepSeekPrompt& prompt) {
                const auto started = std::chrono::steady_clock::now();
                try {
                    std::string result = deepseek_client.complete(prompt);
                    deepseek_latencies.push_back(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started));
                    return result;
                } catch (...) {
                    deepseek_latencies.push_back(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started));
                    throw;
                }
            }};

        exchange::BinanceAlphaMarketFeed feed;
        feed.start();
        try {
            const exchange::ExternalMarketState initial_market =
                wait_for_fresh_market(feed, 30s);
            std::cout << "live Binance Alpha snapshot: "
                      << format_external_market(initial_market) << '\n';

            TemporaryDirectory temporary_directory;
            const std::string wal_path = temporary_directory.wal_path();
            const exchange::TradingBootstrapConfig bootstrap =
                bootstrap_config();
            std::unique_ptr<exchange::TradingRuntime> runtime =
                exchange::TradingRuntime::create_durable(
                    instrument,
                    wal_path,
                    bootstrap);
            const exchange::TradingRuntime& runtime_view = *runtime;
            const exchange::TradingResponse liquidity =
                runtime->executor().execute(exchange::TradingRequest{
                    1,
                    liquidity_account_id,
                    exchange::SubmitTradingRequest{
                        exchange::Side::Sell,
                        100,
                        5}});
            if (liquidity.result != exchange::TradingResult::Accepted
                || !liquidity.assigned_order_id.has_value()) {
                throw std::logic_error(
                    "Failed to create durable internal liquidity order");
            }
            std::cout << "internal setup: durable sell order_id="
                      << *liquidity.assigned_order_id
                      << " price=100 quantity=5\n";

            exchange::AgentRegistry registry;
            if (!registry.register_agent({agent_id, account_id})) {
                throw std::logic_error("Failed to register smoke Agent");
            }
            exchange::AgentObservationService observations{
                registry,
                runtime_view.accounts(),
                runtime->reservations(),
                runtime->order_book(),
                instrument};
            exchange::TradingRequestAgentExecutionAdapter execution{
                registry,
                runtime->executor(),
                2};

            exchange::AgentEconomicProfile profile;
            profile.max_order_quantity = 5;
            profile.max_order_notional = 5'000;
            profile.max_base_position = 20;
            profile.max_buy_price = 1'000;
            profile.min_sell_price = 1;
            const exchange::AgentPreferenceProfile preference{
                15,
                120,
                10};
            exchange::AgentRuntime agent_runtime(
                {{agent_id,
                  &deepseek_provider,
                  exchange::AssetTargetObjective{instrument.base_asset, 15},
                  profile,
                  preference}},
                observations,
                execution,
                instrument,
                &feed);

            for (std::size_t turn = 0; turn < turn_count; ++turn) {
                agent_runtime.run_step_at(system_time_millis());
            }
            feed.stop();

            if (agent_runtime.trace().size() != turn_count
                || deepseek_latencies.size() != turn_count) {
                throw std::logic_error(
                    "Smoke runtime did not record every Agent turn");
            }
            for (std::size_t index = 0;
                 index < agent_runtime.trace().size();
                 ++index) {
                print_turn(
                    agent_runtime.trace()[index],
                    deepseek_latencies[index]);
            }

            const exchange::PerAgentExperimentMetrics* metrics =
                agent_runtime.metrics().find_agent(agent_id);
            if (metrics == nullptr) {
                throw std::logic_error("Smoke Agent metrics are missing");
            }
            std::cout << "metrics turns=" << metrics->turns
                      << " submits=" << metrics->proposed_submits
                      << " cancels=" << metrics->proposed_cancels
                      << " holds=" << metrics->holds
                      << " structural_rejects="
                      << metrics->structural_rejections
                      << " economic_rejects="
                      << metrics->economic_constraint_rejections
                      << " exchange_rejects="
                      << metrics->execution_rejections
                      << " successful_executions="
                      << metrics->successful_executions
                      << " proposed_quantity="
                      << metrics->total_proposed_quantity
                      << " accepted_quantity="
                      << metrics->total_accepted_quantity
                      << " cumulative_utility_delta="
                      << metrics->cumulative_utility_delta
                      << " positive_utility_turns="
                      << metrics->positive_utility_turns
                      << " negative_utility_turns="
                      << metrics->negative_utility_turns
                      << " zero_utility_turns="
                      << metrics->zero_utility_turns << '\n';

            const exchange::WalScanResult wal =
                exchange::scan_execution_wal(
                    read_file(wal_path),
                    instrument,
                    exchange::calculate_bootstrap_fingerprint(bootstrap));
            if (wal.status != exchange::WalScanStatus::CleanEof) {
                throw std::runtime_error(
                    "Smoke WAL did not scan to a clean EOF");
            }
            std::size_t durable_submits = 0;
            std::size_t durable_cancels = 0;
            std::size_t agent_wal_records = 0;
            for (const exchange::WalRecord& record : wal.records) {
                std::visit(
                    [&](const auto& command) {
                        using Command = std::decay_t<decltype(command)>;
                        if constexpr (std::is_same_v<
                                          Command,
                                          exchange::SubmitExecutionCommand>) {
                            ++durable_submits;
                        } else {
                            ++durable_cancels;
                        }
                        if (command.account_id == account_id) {
                            ++agent_wal_records;
                        }
                    },
                    record.command);
            }
            std::cout << "wal records=" << wal.records.size()
                      << " durable_submits=" << durable_submits
                      << " durable_cancels=" << durable_cancels
                      << " agent_wal_records=" << agent_wal_records << '\n'
                      << "Binance feed shutdown=clean; temporary WAL cleanup="
                         "automatic\n";
        } catch (...) {
            feed.stop();
            throw;
        }
        return 0;
    }
}  // namespace

int main() {
    try {
        return run_smoke();
    } catch (const std::exception& error) {
        std::cerr << "exchange_agent_live_smoke error: "
                  << error.what() << '\n';
        return 1;
    }
}
