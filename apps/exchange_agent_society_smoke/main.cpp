#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
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

#include "agent/exchange/agent_observation_service.hpp"
#include "agent/exchange/trading_request_agent_execution_adapter.hpp"
#include "agent/provider/binance_alpha/binance_alpha_market_feed.hpp"
#include "agent/provider/deepseek/deepseek_client.hpp"
#include "agent/provider/deepseek/deepseek_decision_provider.hpp"
#include "agent/runtime/agent_runtime.hpp"
#include "durability/execution_wal.hpp"
#include "execution/trading_bootstrap.hpp"
#include "execution/trading_request.hpp"
#include "execution/trading_runtime.hpp"
#include "final_observations.hpp"

namespace {
    using namespace std::chrono_literals;

    constexpr exchange::InstrumentContext instrument{20, 10, 1, 1, 1};
    constexpr std::size_t agent_count = 3;
    constexpr std::size_t default_steps = 20;
    constexpr std::size_t maximum_steps = 30;
    constexpr exchange::AgentId agent_a = 101;
    constexpr exchange::AgentId agent_b = 202;
    constexpr exchange::AgentId agent_c = 303;
    constexpr exchange::AccountId account_a = 1;
    constexpr exchange::AccountId account_b = 2;
    constexpr exchange::AccountId account_c = 3;
    constexpr exchange::AccountId liquidity_account = 4;

    enum class MarketMode {
        Live,
        None,
    };

    struct ExperimentConfig {
        std::size_t steps{default_steps};
        MarketMode market_mode{MarketMode::Live};
    };

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string pattern =
                (std::filesystem::temp_directory_path()
                 / "exchange-agent-society-smoke-XXXXXX")
                    .string();
            std::vector<char> writable(pattern.begin(), pattern.end());
            writable.push_back('\0');
            const char* created = ::mkdtemp(writable.data());
            if (created == nullptr) {
                throw std::runtime_error("failed to create experiment directory");
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

    std::string_view market_mode_name(MarketMode mode) {
        return mode == MarketMode::Live ? "live" : "none";
    }

    ExperimentConfig parse_config(int argc, char** argv) {
        ExperimentConfig config;
        bool has_steps = false;
        bool has_market = false;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    "usage: exchange_agent_society_smoke "
                    "[--steps 1..30] [--market live|none]");
            }
            const std::string_view option{argv[index]};
            const std::string_view value{argv[index + 1]};
            if (option == "--steps" && !has_steps) {
                const auto parsed = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    config.steps);
                if (parsed.ec != std::errc{}
                    || parsed.ptr != value.data() + value.size()
                    || config.steps == 0 || config.steps > maximum_steps) {
                    throw std::invalid_argument(
                        "steps must be an integer in [1, 30]");
                }
                has_steps = true;
            } else if (option == "--market" && !has_market) {
                if (value == "live") {
                    config.market_mode = MarketMode::Live;
                } else if (value == "none") {
                    config.market_mode = MarketMode::None;
                } else {
                    throw std::invalid_argument(
                        "market must be 'live' or 'none'");
                }
                has_market = true;
            } else {
                throw std::invalid_argument(
                    "usage: exchange_agent_society_smoke "
                    "[--steps 1..30] [--market live|none]");
            }
        }
        return config;
    }

    exchange::ExternalMarketState wait_for_fresh_market(
        const exchange::BinanceAlphaMarketFeed& feed,
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            exchange::ExternalMarketState state = feed.latest(
                system_time_millis());
            if (state.freshness == exchange::ExternalMarketFreshness::Fresh) {
                return state;
            }
            if (!feed.metadata().has_value() && !feed.last_error().empty()) {
                throw std::runtime_error(
                    "Binance Alpha initialization failed: "
                    + feed.last_error());
            }
            std::this_thread::sleep_for(100ms);
        }
        std::string message = "timed out waiting for fresh Binance Alpha data";
        if (!feed.last_error().empty()) {
            message += ": " + feed.last_error();
        }
        throw std::runtime_error(std::move(message));
    }

    std::string_view contract_state_name(exchange::ContractState state) {
        using State = exchange::ContractState;
        switch (state) {
            case State::Proposed: return "Proposed";
            case State::Accepted: return "Accepted";
            case State::Rejected: return "Rejected";
            case State::Fulfilled: return "Fulfilled";
            case State::Settled: return "Settled";
        }
        throw std::logic_error("unknown contract state");
    }

    std::string_view structural_name(exchange::AgentActionValidationResult value) {
        using Value = exchange::AgentActionValidationResult;
        switch (value) {
            case Value::Valid: return "valid";
            case Value::InvalidSide: return "invalid_side";
            case Value::InvalidPrice: return "invalid_price";
            case Value::InvalidQuantity: return "invalid_quantity";
            case Value::InvalidOrderId: return "invalid_order_id";
            case Value::CancelTargetNotActive: return "cancel_target_not_active";
            case Value::InvalidFinancialValue: return "invalid_financial_value";
            case Value::InvalidContractCounterparty:
                return "invalid_contract_counterparty";
            case Value::InvalidContractParties: return "invalid_contract_parties";
            case Value::InvalidContractPayment: return "invalid_contract_payment";
            case Value::InvalidContractResource: return "invalid_contract_resource";
            case Value::InvalidContractQuantity: return "invalid_contract_quantity";
            case Value::InvalidContractId: return "invalid_contract_id";
        }
        throw std::logic_error("unknown structural result");
    }

    std::string_view economic_name(exchange::AgentEconomicConstraintResult value) {
        using Value = exchange::AgentEconomicConstraintResult;
        switch (value) {
            case Value::Allowed: return "allowed";
            case Value::OrderQuantityExceeded: return "order_quantity_exceeded";
            case Value::OrderNotionalExceeded: return "order_notional_exceeded";
            case Value::BasePositionExceeded: return "base_position_exceeded";
            case Value::BuyPriceExceeded: return "buy_price_exceeded";
            case Value::SellPriceBelowMinimum: return "sell_price_below_minimum";
        }
        throw std::logic_error("unknown economic result");
    }

    std::string_view contract_result_name(exchange::ContractResult value) {
        using Value = exchange::ContractResult;
        switch (value) {
            case Value::Success: return "success";
            case Value::ContractNotFound: return "contract_not_found";
            case Value::InvalidTerms: return "invalid_terms";
            case Value::InvalidTransition: return "invalid_transition";
            case Value::UnauthorizedActor: return "unauthorized_actor";
            case Value::AccountNotFound: return "account_not_found";
            case Value::InsufficientFunds: return "insufficient_funds";
            case Value::BalanceOverflow: return "balance_overflow";
            case Value::ContractIdExhausted: return "contract_id_exhausted";
        }
        throw std::logic_error("unknown contract result");
    }

    std::string_view submit_result_name(exchange::AgentSubmitStatus value) {
        using Value = exchange::AgentSubmitStatus;
        switch (value) {
            case Value::Accepted: return "accepted";
            case Value::AccountNotFound: return "account_not_found";
            case Value::InsufficientFunds: return "insufficient_funds";
            case Value::DuplicateOrder: return "duplicate_order";
            case Value::InvalidOrder: return "invalid_order";
            case Value::CounterpartyNotAccountBacked:
                return "counterparty_not_account_backed";
        }
        throw std::logic_error("unknown submit result");
    }

    std::string_view cancel_result_name(exchange::AgentCancelStatus value) {
        using Value = exchange::AgentCancelStatus;
        switch (value) {
            case Value::Cancelled: return "cancelled";
            case Value::AccountNotFound: return "account_not_found";
            case Value::NotFound: return "not_found";
            case Value::NotOwner: return "not_owner";
        }
        throw std::logic_error("unknown cancel result");
    }

    std::string format_action(const std::optional<exchange::AgentAction>& action) {
        if (!action.has_value()) {
            return "none";
        }
        return std::visit(
            [](const auto& value) {
                using Action = std::decay_t<decltype(value)>;
                std::ostringstream output;
                if constexpr (std::is_same_v<Action, exchange::SubmitOrderAction>) {
                    output << "submit_" << (value.side == exchange::Side::Buy ? "buy" : "sell")
                           << " price=" << value.price
                           << " quantity=" << value.quantity;
                } else if constexpr (std::is_same_v<Action, exchange::CancelOrderAction>) {
                    output << "cancel order_id=" << value.order_id;
                } else if constexpr (std::is_same_v<Action, exchange::ProposeContractAction>) {
                    output << "propose_contract counterparty=" << value.counterparty;
                } else if constexpr (std::is_same_v<Action, exchange::AcceptContractAction>) {
                    output << "accept_contract contract_id=" << value.contract_id;
                } else if constexpr (std::is_same_v<Action, exchange::RejectContractAction>) {
                    output << "reject_contract contract_id=" << value.contract_id;
                } else if constexpr (std::is_same_v<Action, exchange::FulfillResourceObligationAction>) {
                    output << "fulfill_resource contract_id=" << value.contract_id;
                } else if constexpr (std::is_same_v<Action, exchange::SettlePaymentObligationAction>) {
                    output << "settle_payment contract_id=" << value.contract_id;
                } else {
                    static_assert(std::is_same_v<Action, exchange::HoldAction>);
                    output << "hold";
                }
                return output.str();
            },
            *action);
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
                if constexpr (std::is_same_v<Result, exchange::SubmitActionResult>) {
                    output << "submit_status=" << submit_result_name(value.status);
                    if (value.status == exchange::AgentSubmitStatus::Accepted) {
                        output << " order_id=" << value.order_id;
                    }
                } else if constexpr (std::is_same_v<Result, exchange::CancelActionResult>) {
                    output << "cancel_status=" << cancel_result_name(value.status);
                } else if constexpr (std::is_same_v<Result, exchange::ContractActionResult>) {
                    output << "contract_status=" << contract_result_name(value.status);
                    if (value.contract_id.has_value()) {
                        output << " contract_id=" << *value.contract_id;
                    }
                } else {
                    static_assert(std::is_same_v<Result, exchange::HoldActionResult>);
                    output << "hold";
                }
                return output.str();
            },
            *result);
    }

    std::string format_balance(const std::optional<exchange::Balance>& balance) {
        if (!balance.has_value()) {
            return "none";
        }
        return std::to_string(balance->available) + "/"
            + std::to_string(balance->reserved);
    }

    std::optional<exchange::Balance> find_balance(
        const exchange::AccountStore::AccountBalances& accounts,
        exchange::AccountId account_id,
        exchange::AssetId asset_id) {
        const auto account = accounts.find(account_id);
        if (account == accounts.end()) {
            return std::nullopt;
        }
        const auto balance = account->second.find(asset_id);
        return balance == account->second.end()
            ? std::nullopt
            : std::optional<exchange::Balance>{balance->second};
    }

    void print_turn(const exchange::AgentTurnRecord& turn) {
        std::cout << "turn step=" << turn.step
                  << " agent=" << turn.agent_id
                  << " action=" << format_action(turn.action)
                  << " structural="
                  << (turn.validation.has_value()
                          ? structural_name(*turn.validation)
                          : "not_reached")
                  << " economic="
                  << (turn.economic_constraint.has_value()
                          ? economic_name(*turn.economic_constraint)
                          : "not_reached")
                  << " execution=" << format_execution(turn.execution_result)
                  << " utility_before="
                  << (turn.utility_before.has_value()
                          ? std::to_string(turn.utility_before->total)
                          : "none")
                  << " utility_after="
                  << (turn.utility_after.has_value()
                          ? std::to_string(turn.utility_after->total)
                          : "none")
                  << " utility_delta="
                  << (turn.utility_delta.has_value()
                          ? std::to_string(*turn.utility_delta)
                          : "none")
                  << " base_after=" << format_balance(turn.post_state.base_balance)
                  << " quote_after=" << format_balance(turn.post_state.quote_balance)
                  << " relevant_contracts_before="
                  << turn.observation.contracts.size();
        if (turn.contract_state_after_action.has_value()) {
            std::cout << " contract_state_after="
                      << contract_state_name(*turn.contract_state_after_action);
        }
        std::cout << '\n';
    }

    exchange::TradingBootstrapConfig bootstrap_config() {
        return exchange::TradingBootstrapConfig{{
            exchange::BootstrapAccount{
                account_a,
                {{instrument.base_asset, {3, 0}},
                 {instrument.quote_asset, {3'000, 0}}}},
            exchange::BootstrapAccount{
                account_b,
                {{instrument.base_asset, {25, 0}},
                 {instrument.quote_asset, {800, 0}}}},
            exchange::BootstrapAccount{
                account_c,
                {{instrument.base_asset, {12, 0}},
                 {instrument.quote_asset, {1'500, 0}}}},
            exchange::BootstrapAccount{
                liquidity_account,
                {{instrument.base_asset, {30, 0}}}},
        }};
    }

    exchange::AgentEconomicProfile profile(
        exchange::Quantity max_quantity,
        exchange::Amount max_notional,
        exchange::Amount max_position,
        exchange::Price max_buy,
        exchange::Price min_sell) {
        exchange::AgentEconomicProfile result;
        result.max_order_quantity = max_quantity;
        result.max_order_notional = max_notional;
        result.max_base_position = max_position;
        result.max_buy_price = max_buy;
        result.min_sell_price = min_sell;
        return result;
    }

    void register_agents(exchange::TradingRuntime& runtime) {
        if (!runtime.agent_registry().register_agent({agent_a, account_a})
            || !runtime.agent_registry().register_agent({agent_b, account_b})
            || !runtime.agent_registry().register_agent({agent_c, account_c})) {
            throw std::logic_error("failed to register experiment Agents");
        }
    }

    std::vector<std::uint8_t> read_file(const std::string& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("failed to open experiment WAL");
        }
        const std::streamsize size = input.tellg();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (size > 0
            && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw std::runtime_error("failed to read experiment WAL");
        }
        return bytes;
    }

    std::map<exchange::ContractId, exchange::Contract> collect_contracts(
        const exchange::ContractStore& contracts) {
        std::map<exchange::ContractId, exchange::Contract> result;
        for (const exchange::AgentId agent : {agent_a, agent_b, agent_c}) {
            for (const exchange::Contract& contract : contracts.find_relevant(agent)) {
                result.emplace(contract.id, contract);
            }
        }
        return result;
    }

    void print_summary(
        const exchange::AgentRuntime& agents,
        const exchange::TradingRuntime& runtime,
        const exchange::AccountStore::AccountBalances& initial_accounts,
        const std::map<exchange::AgentId, exchange::AgentObservation>& final_observations) {
        const exchange::AgentExperimentMetrics& metrics = agents.metrics();
        std::uint64_t proposed_cancels = 0;
        std::uint64_t decision_failures = 0;
        std::cout << "per-agent summary\n";
        for (const auto& [agent_id, metric] : metrics.per_agent()) {
            const auto& final = final_observations.at(agent_id);
            const std::optional<exchange::UtilityValue> final_utility =
                final.preference_profile.has_value()
                ? std::optional<exchange::UtilityValue>{
                      exchange::evaluate_agent_utility(
                          final, *final.preference_profile).total}
                : std::nullopt;
            const auto registration = runtime.agent_registry().find(agent_id);
            if (!registration.has_value()) {
                throw std::logic_error("experiment Agent is not registered");
            }
            proposed_cancels += metric.proposed_cancels;
            decision_failures += metric.decision_failures;
            std::cout << "agent=" << agent_id
                      << " turns=" << metric.turns
                      << " trading_proposals=" << metric.proposed_submits
                      << " contract_proposals=" << metric.contract_proposals
                      << " accepts=" << metric.contract_accepts
                      << " rejects=" << metric.contract_rejects
                      << " fulfillments=" << metric.successful_contract_fulfillments
                      << " settlements=" << metric.successful_contract_settlements
                      << " execution_rejections=" << metric.execution_rejections
                      << " initial_base="
                      << format_balance(find_balance(
                             initial_accounts,
                             registration->account_id,
                             instrument.base_asset))
                      << " final_base="
                      << format_balance(final.base_balance)
                      << " initial_quote="
                      << format_balance(find_balance(
                             initial_accounts,
                             registration->account_id,
                             instrument.quote_asset))
                      << " final_quote="
                      << format_balance(final.quote_balance)
                      << " active_orders=" << final.active_orders.size()
                      << " final_utility="
                      << (final_utility.has_value()
                              ? std::to_string(*final_utility)
                              : "none")
                      << " cumulative_utility_delta="
                      << metric.cumulative_utility_delta << '\n';
        }
        const exchange::SocietyExperimentMetrics society = metrics.society();
        std::cout << "actions hold=" << society.total_holds
                  << " submit_order="
                  << society.total_buys + society.total_sells
                  << " cancel_order=" << proposed_cancels
                  << " contract_propose=" << society.total_contract_proposals
                  << " contract_accept=" << society.total_contract_accepts
                  << " contract_reject=" << society.total_contract_rejects
                  << " fulfill="
                  << society.total_contract_fulfillment_attempts
                  << " settle=" << society.total_contract_settlement_attempts
                  << " decision_failed=" << decision_failures
                  << " structural_rejected="
                  << society.total_structural_rejections
                  << " economic_rejected="
                  << society.total_economic_constraint_rejections
                  << " execution_rejected=" << society.total_exchange_rejections
                  << '\n';
        std::cout << "society turns=" << society.total_turns
                  << " successful_executions=" << society.total_successful_executions
                  << " contract_proposals=" << society.total_contract_proposals
                  << " accepts=" << society.total_contract_accepts
                  << " rejects=" << society.total_contract_rejects
                  << " fulfillments=" << society.total_successful_contract_fulfillments
                  << " settlements=" << society.total_successful_contract_settlements
                  << " settlement_rejections="
                  << society.total_contract_settlement_rejections << '\n';
        std::cout << "contract pair interactions\n";
        for (const auto& [pair, interaction] : metrics.contract_pair_interactions()) {
            std::cout << "proposer=" << pair.first
                      << " counterparty=" << pair.second
                      << " proposals=" << interaction.proposal_attempts
                      << " accepted=" << interaction.accepted_contracts << '\n';
        }

        std::map<exchange::ContractState, std::size_t> states;
        const auto contracts = collect_contracts(runtime.contracts());
        for (const auto& [id, contract] : contracts) {
            static_cast<void>(id);
            ++states[contract.state];
        }
        std::cout << "contract states Proposed=" << states[exchange::ContractState::Proposed]
                  << " Accepted=" << states[exchange::ContractState::Accepted]
                  << " Rejected=" << states[exchange::ContractState::Rejected]
                  << " Fulfilled=" << states[exchange::ContractState::Fulfilled]
                  << " Settled=" << states[exchange::ContractState::Settled] << '\n';
        std::cout << "contract trace\n";
        for (const auto& [id, contract] : contracts) {
            std::cout << "Contract #" << id
                      << ": proposer=" << contract.proposer
                      << " counterparty=" << contract.counterparty
                      << " payer=" << contract.terms.payer
                      << " payee=" << contract.terms.payee
                      << " payment=" << contract.terms.quote_payment_amount
                      << " resource=ComputeCredit"
                      << " quantity=" << contract.terms.resource_quantity
                      << " state=" << contract_state_name(contract.state)
                      << " resource_fulfilled="
                      << (contract.resource_delivery_obligation.fulfilled ? "true" : "false")
                      << " payment_fulfilled="
                      << (contract.payment_obligation.fulfilled ? "true" : "false")
                      << '\n';
        }

        std::size_t settlement_entries = 0;
        exchange::Amount settled_quote_transferred = 0;
        for (const exchange::LedgerEntry& entry : runtime.ledger().entries()) {
            const auto* metadata = std::get_if<
                exchange::ContractSettlementLedgerMetadata>(
                &entry.transaction.metadata);
            if (metadata != nullptr) {
                ++settlement_entries;
                const auto contract = runtime.contracts().find(
                    metadata->contract_id);
                if (!contract.has_value()) {
                    throw std::logic_error(
                        "settlement Ledger entry has no contract");
                }
                if (contract->terms.quote_payment_amount
                    > std::numeric_limits<exchange::Amount>::max()
                          - settled_quote_transferred) {
                    throw std::overflow_error(
                        "settled quote total overflow");
                }
                settled_quote_transferred +=
                    contract->terms.quote_payment_amount;
            }
        }
        std::cout << "ledger entries=" << runtime.ledger().entries().size()
                  << " settlement_entries=" << settlement_entries
                  << " settled_quote_transferred="
                  << settled_quote_transferred << '\n';
        for (const auto& [id, contract] : contracts) {
            if (contract.state != exchange::ContractState::Settled) {
                continue;
            }
            const auto payer = runtime.agent_registry().find(
                contract.payment_obligation.debtor);
            const auto payee = runtime.agent_registry().find(
                contract.payment_obligation.creditor);
            if (!payer.has_value() || !payee.has_value()) {
                throw std::logic_error("settled contract has no registered party");
            }
            bool ledger_found = false;
            for (const exchange::LedgerEntry& entry : runtime.ledger().entries()) {
                const auto* metadata = std::get_if<
                    exchange::ContractSettlementLedgerMetadata>(
                    &entry.transaction.metadata);
                if (metadata != nullptr && metadata->contract_id == id) {
                    ledger_found = true;
                    break;
                }
            }
            if (!ledger_found) {
                throw std::logic_error("settled contract has no settlement Ledger entry");
            }
            std::cout << "settlement audit contract=" << id
                      << " payer_quote="
                      << format_balance(
                             runtime.accounts().find_balance(
                                 payer->account_id,
                                 instrument.quote_asset))
                      << " payee_quote="
                      << format_balance(
                             runtime.accounts().find_balance(
                                 payee->account_id,
                                 instrument.quote_asset))
                      << " ledger_entry=true state=Settled\n";
        }
    }

    int run_experiment(int argc, char** argv) {
        const ExperimentConfig config = parse_config(argc, argv);
        std::cout << "starting Agent society experiment agents=" << agent_count
                  << " steps=" << config.steps
                  << " total_turns=" << config.steps * agent_count
                  << " market_mode=" << market_mode_name(config.market_mode)
                  << " provider=DeepSeek\n";

        exchange::DeepSeekClientConfig deepseek_config =
            exchange::deepseek_config_from_environment();
        deepseek_config.timeout = 10s;
        exchange::DeepSeekClient deepseek_client{std::move(deepseek_config)};
        const auto complete = [&deepseek_client](const exchange::DeepSeekPrompt& prompt) {
            return deepseek_client.complete(prompt);
        };
        exchange::DeepSeekDecisionProvider provider_a{complete};
        exchange::DeepSeekDecisionProvider provider_b{complete};
        exchange::DeepSeekDecisionProvider provider_c{complete};

        std::unique_ptr<exchange::BinanceAlphaMarketFeed> feed;
        if (config.market_mode == MarketMode::Live) {
            feed = std::make_unique<exchange::BinanceAlphaMarketFeed>();
            feed->start();
            try {
                const exchange::ExternalMarketState market =
                    wait_for_fresh_market(*feed, 30s);
                std::cout << "market_mode=live external_symbol="
                          << market.symbol << '\n';
            } catch (...) {
                feed->stop();
                throw;
            }
        } else {
            std::cout << "market_mode=none external_market=unavailable\n";
        }
        try {
            TemporaryDirectory directory;
            const std::string wal_path = directory.wal_path();
            const exchange::TradingBootstrapConfig bootstrap = bootstrap_config();
            exchange::AccountStore::AccountBalances final_accounts;
            std::map<exchange::ContractId, exchange::Contract> final_contracts;
            std::vector<exchange::LedgerEntry> final_ledger;
            {
                std::unique_ptr<exchange::TradingRuntime> runtime =
                    exchange::TradingRuntime::create_durable(
                        instrument, wal_path, bootstrap);
                const exchange::TradingResponse liquidity = runtime->executor().execute(
                    exchange::TradingRequest{
                        1,
                        liquidity_account,
                        exchange::SubmitTradingRequest{
                            exchange::Side::Sell,
                            100,
                            20}});
                if (liquidity.result != exchange::TradingResult::Accepted) {
                    throw std::logic_error("failed to create internal liquidity");
                }
                register_agents(*runtime);
                const exchange::TradingRuntime& view = *runtime;
                const exchange::AccountStore::AccountBalances initial_accounts =
                    view.accounts().entries();
                exchange::AgentObservationService observations{
                    runtime->agent_registry(),
                    view.accounts(),
                    runtime->reservations(),
                    runtime->order_book(),
                    runtime->contracts(),
                    instrument};
                exchange::TradingRequestAgentExecutionAdapter execution{
                    runtime->agent_registry(),
                    runtime->executor(),
                    runtime->contract_executor(),
                    2};
                const std::vector<exchange::AgentRuntimeParticipant> participants{
                     {agent_a,
                      &provider_a,
                      exchange::AssetTargetObjective{instrument.base_asset, 18},
                      profile(4, 2'000, 22, 180, 60),
                      exchange::AgentPreferenceProfile{18, 140, 3}},
                     {agent_b,
                      &provider_b,
                      exchange::AssetTargetObjective{instrument.base_asset, 8},
                      profile(6, 2'500, 32, 150, 40),
                      exchange::AgentPreferenceProfile{8, 85, 2}},
                     {agent_c,
                      &provider_c,
                      exchange::AssetTargetObjective{instrument.base_asset, 12},
                      profile(3, 1'200, 18, 125, 75),
                      exchange::AgentPreferenceProfile{12, 115, 11}}};
                exchange::AgentRuntime agents(
                    participants,
                    observations,
                    execution,
                    instrument,
                    feed.get());

                std::size_t printed = 0;
                for (std::size_t step = 0; step < config.steps; ++step) {
                    agents.run_step_at(system_time_millis());
                    for (; printed < agents.trace().size(); ++printed) {
                        print_turn(agents.trace()[printed]);
                    }
                    if (runtime->executor().poisoned()
                        || runtime->contract_executor().poisoned()) {
                        throw std::runtime_error("durable runtime became poisoned");
                    }
                }
                const auto final_observations =
                    exchange::society_smoke::capture_final_observations(
                        observations, participants, agents.current_step());
                print_summary(agents, *runtime, initial_accounts, final_observations);
                final_accounts = view.accounts().entries();
                final_contracts = collect_contracts(runtime->contracts());
                final_ledger = runtime->ledger().entries();
            }
            if (feed != nullptr) {
                feed->stop();
            }

            const exchange::WalScanResult wal = exchange::scan_execution_wal(
                read_file(wal_path),
                instrument,
                exchange::calculate_bootstrap_fingerprint(bootstrap));
            if (wal.status != exchange::WalScanStatus::CleanEof) {
                throw std::runtime_error("experiment WAL did not reach clean EOF");
            }
            std::unique_ptr<exchange::TradingRuntime> recovered =
                exchange::TradingRuntime::create_durable(
                    instrument, wal_path, bootstrap);
            register_agents(*recovered);
            if (static_cast<const exchange::TradingRuntime&>(*recovered)
                    .accounts().entries()
                    != final_accounts
                || collect_contracts(recovered->contracts()) != final_contracts
                || recovered->ledger().entries() != final_ledger) {
                throw std::runtime_error("experiment recovery state mismatch");
            }
            std::cout << "recovery=verified wal_records=" << wal.records.size()
                      << " temporary_wal_cleanup=automatic\n";
        } catch (...) {
            if (feed != nullptr) {
                feed->stop();
            }
            throw;
        }
        return 0;
    }
}  // namespace

int main(int argc, char** argv) {
    try {
        return run_experiment(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "exchange_agent_society_smoke error: " << error.what() << '\n';
        return 1;
    }
}
