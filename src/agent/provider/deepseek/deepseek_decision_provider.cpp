#include "agent/provider/deepseek/deepseek_decision_provider.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace exchange {
    namespace {
        using Json = nlohmann::json;

        void require_exact_fields(
            const Json& value,
            const std::set<std::string>& expected) {
            std::set<std::string> actual;
            for (const auto& [key, unused] : value.items()) {
                static_cast<void>(unused);
                actual.insert(key);
            }
            if (actual != expected) {
                throw DeepSeekDecisionError(
                    "DeepSeek action fields do not match the action schema");
            }
        }

        std::string required_string(const Json& value, const char* key) {
            const auto field = value.find(key);
            if (field == value.end() || !field->is_string()) {
                throw DeepSeekDecisionError(
                    "DeepSeek action requires a string field");
            }
            return field->get<std::string>();
        }

        std::int64_t required_positive_int64(
            const Json& value,
            const char* key) {
            const auto field = value.find(key);
            if (field == value.end()
                || (!field->is_number_integer()
                    && !field->is_number_unsigned())) {
                throw DeepSeekDecisionError(
                    "DeepSeek action requires an integer field");
            }

            if (field->is_number_unsigned()) {
                const auto number = field->get<std::uint64_t>();
                if (number == 0
                    || number > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max())) {
                    throw DeepSeekDecisionError(
                        "DeepSeek action integer is out of range");
                }
                return static_cast<std::int64_t>(number);
            }

            const auto number = field->get<std::int64_t>();
            if (number <= 0) {
                throw DeepSeekDecisionError(
                    "DeepSeek action integer must be positive");
            }
            return number;
        }

        std::uint64_t required_positive_uint64(
            const Json& value,
            const char* key) {
            const auto field = value.find(key);
            if (field == value.end()
                || (!field->is_number_integer()
                    && !field->is_number_unsigned())) {
                throw DeepSeekDecisionError(
                    "DeepSeek action requires an integer field");
            }
            if (field->is_number_unsigned()) {
                const std::uint64_t number = field->get<std::uint64_t>();
                if (number == 0) {
                    throw DeepSeekDecisionError(
                        "DeepSeek action integer must be positive");
                }
                return number;
            }
            const std::int64_t number = field->get<std::int64_t>();
            if (number <= 0) {
                throw DeepSeekDecisionError(
                    "DeepSeek action integer must be positive");
            }
            return static_cast<std::uint64_t>(number);
        }

        OrderId required_order_id(const Json& value) {
            const std::int64_t order_id = required_positive_int64(
                value,
                "order_id");
            return static_cast<OrderId>(order_id);
        }

        ContractId required_contract_id(const Json& value) {
            const ContractId contract_id = required_positive_uint64(
                value,
                "contract_id");
            if (contract_id == std::numeric_limits<ContractId>::max()) {
                throw DeepSeekDecisionError(
                    "DeepSeek contract ID is out of range");
            }
            return contract_id;
        }

        std::string_view contract_state_name(ContractState state) {
            switch (state) {
                case ContractState::Proposed: return "proposed";
                case ContractState::Accepted: return "accepted";
                case ContractState::Rejected: return "rejected";
                case ContractState::Fulfilled: return "fulfilled";
                case ContractState::Settled: return "settled";
            }
            throw std::logic_error("Unknown contract state");
        }

        std::string_view resource_name(ResourceKind resource) {
            switch (resource) {
                case ResourceKind::ComputeCredit: return "compute_credit";
            }
            throw std::logic_error("Unknown contract resource");
        }

        void append_balance(
            std::ostringstream& output,
            std::string_view name,
            const std::optional<Balance>& balance) {
            output << name << ": ";
            if (!balance.has_value()) {
                output << "null\n";
                return;
            }
            output << "available=" << balance->available
                   << ", reserved=" << balance->reserved << '\n';
        }

        void append_price(
            std::ostringstream& output,
            std::string_view name,
            const std::optional<Price>& price) {
            output << name << ": ";
            if (price.has_value()) {
                output << *price;
            } else {
                output << "null";
            }
            output << '\n';
        }
    }  // namespace

    DeepSeekPrompt build_deepseek_prompt(
        const AgentObservation& observation) {
        DeepSeekPrompt prompt;
        prompt.system =
            "You propose exactly one action for your own exchange agent. "
            "The deterministic exchange validates and executes it. You cannot "
            "modify balances, reservations, orders, settlement, or runtime "
            "state directly. You cannot call tools. Return one JSON object "
            "with no Markdown or prose. "
            "External market data is reference environment context only. "
            "You are not trading on Binance; every legal action targets the "
            "internal exchange. Economic limits in the observation are hard "
            "trading-action constraints enforced by the deterministic runtime. "
            "Contracts are internal Agent Native economic agreements. "
            "ComputeCredit is a synthetic internal resource. Proposing, "
            "accepting, or fulfilling a contract does not transfer money. "
            "Only an Accepted contract can be fulfilled, and only by the "
            "resource-delivery debtor. Fulfillment declares the complete "
            "agreed ComputeCredit quantity delivered; it does not pay the "
            "contract. Only a Fulfilled contract can be settled, and only "
            "by its payment debtor. Settlement transfers the complete "
            "internal quote payment from available balance to the payee; "
            "reserved quote is not spendable. Insufficient available quote "
            "rejects settlement. Settlement completes the contract without "
            "any blockchain, wallet, or x402 payment. The runtime remains "
            "authoritative. "
            "Legal schemas are: "
            "{\"action\":\"submit_order\",\"side\":\"buy|sell\","
            "\"price\":positive_integer,\"quantity\":positive_integer}, "
            "{\"action\":\"cancel_order\",\"order_id\":positive_integer}, "
            "{\"action\":\"propose_contract\","
            "\"counterparty\":positive_integer,"
            "\"payer\":positive_integer,\"payee\":positive_integer,"
            "\"payment_amount\":positive_integer,"
            "\"resource\":\"compute_credit\","
            "\"resource_quantity\":positive_integer}, "
            "{\"action\":\"accept_contract\","
            "\"contract_id\":positive_integer}, "
            "{\"action\":\"reject_contract\","
            "\"contract_id\":positive_integer}, "
            "{\"action\":\"fulfill_resource_obligation\","
            "\"contract_id\":positive_integer}, "
            "{\"action\":\"settle_payment_obligation\","
            "\"contract_id\":positive_integer}, "
            "or {\"action\":\"hold\"}. Include exactly the fields in the "
            "selected schema.";

        std::ostringstream user;
        user << "Step: " << observation.world.step << '\n'
             << "Agent ID: " << observation.agent_id << '\n'
             << "Account ID: " << observation.account_id << '\n';
        append_balance(user, "Base balance", observation.base_balance);
        append_balance(user, "Quote balance", observation.quote_balance);
        append_price(
            user,
            "Best bid",
            observation.world.internal_market.best_bid);
        append_price(
            user,
            "Best ask",
            observation.world.internal_market.best_ask);
        user << "Active own orders:";
        if (observation.active_orders.empty()) {
            user << " none\n";
        } else {
            user << '\n';
            for (const ObservedOrder& order : observation.active_orders) {
                user << "- id=" << order.order_id
                     << ", side="
                     << (order.side == Side::Buy ? "buy" : "sell")
                     << ", price=" << order.price
                     << ", remaining_quantity="
                     << order.remaining_quantity << '\n';
            }
        }
        user << "Relevant contracts:";
        if (observation.contracts.empty()) {
            user << " none\n";
        } else {
            user << '\n';
            for (const Contract& contract : observation.contracts) {
                user << "- id=" << contract.id
                     << ", proposer=" << contract.proposer
                     << ", counterparty=" << contract.counterparty
                     << ", state=" << contract_state_name(contract.state)
                     << ", payer=" << contract.terms.payer
                     << ", payee=" << contract.terms.payee
                     << ", payment_amount="
                     << contract.terms.quote_payment_amount
                     << ", resource="
                     << resource_name(contract.terms.resource)
                     << ", resource_quantity="
                     << contract.terms.resource_quantity
                     << ", resource_debtor="
                     << contract.resource_delivery_obligation.debtor
                     << ", resource_creditor="
                     << contract.resource_delivery_obligation.creditor
                     << ", payment_fulfilled="
                     << (contract.payment_obligation.fulfilled
                             ? "true"
                             : "false")
                     << ", resource_fulfilled="
                     << (contract.resource_delivery_obligation.fulfilled
                             ? "true"
                             : "false")
                     << '\n';
            }
        }
        user << "External market: ";
        if (!observation.world.external_market.has_value()
            || observation.world.external_market->freshness
                   != ExternalMarketFreshness::Fresh
            || !observation.world.external_market->latest_trade_price
                    .has_value()
            || !observation.world.external_market->best_bid.has_value()
            || !observation.world.external_market->best_ask.has_value()) {
            user << "unavailable\n";
        } else {
            const ExternalMarketState& external =
                *observation.world.external_market;
            user << "source=Binance Alpha"
                 << ", symbol=" << external.symbol
                 << ", latest_trade="
                 << format_external_price(*external.latest_trade_price)
                 << ", best_bid="
                 << format_external_price(*external.best_bid)
                 << ", best_ask="
                 << format_external_price(*external.best_ask)
                 << ", event_timestamp_ms="
                 << external.event_timestamp_ms << '\n';
        }
        const AgentEconomicProfile& profile =
            observation.economic_profile;
        user << "Economic hard limits: max_order_quantity="
             << profile.max_order_quantity
             << ", max_order_notional_quote_atomic="
             << profile.max_order_notional
             << ", max_base_position_atomic="
             << profile.max_base_position
             << ", max_buy_price=";
        if (profile.max_buy_price.has_value()) {
            user << *profile.max_buy_price;
        } else {
            user << "null";
        }
        user << ", min_sell_price=";
        if (profile.min_sell_price.has_value()) {
            user << *profile.min_sell_price;
        } else {
            user << "null";
        }
        user << '\n';
        user << "Utility preference: ";
        if (!observation.preference_profile.has_value()) {
            user << "null\n";
        } else {
            const AgentPreferenceProfile& preference =
                *observation.preference_profile;
            user << "quote_unit_value=1"
                 << ", target_base_inventory="
                 << preference.target_base_inventory
                 << ", base_unit_value="
                 << preference.base_unit_value
                 << ", inventory_deviation_penalty_per_unit="
                 << preference.inventory_deviation_penalty_per_unit
                 << ". Runtime evaluates utility before and after the "
                    "action; utility does not change action admissibility.\n";
        }
        user << "Objective: ";
        if (!observation.objective.has_value()) {
            user << "null\n";
        } else {
            const ObjectiveProgress& objective = *observation.objective;
            user << "asset=" << objective.asset_id
                 << ", current=" << objective.current_amount
                 << ", target=" << objective.target_amount
                 << ", achieved="
                 << (objective.achieved ? "true" : "false") << '\n';
        }
        user << "Choose one legal action.";
        prompt.user = user.str();
        return prompt;
    }

    AgentAction parse_deepseek_action(std::string_view content) {
        try {
            const Json value = Json::parse(content);
            if (!value.is_object()) {
                throw DeepSeekDecisionError(
                    "DeepSeek action must be a JSON object");
            }

            const std::string action = required_string(value, "action");
            if (action == "hold") {
                require_exact_fields(value, {"action"});
                return HoldAction{};
            }
            if (action == "cancel_order") {
                require_exact_fields(value, {"action", "order_id"});
                return CancelOrderAction{required_order_id(value)};
            }
            if (action == "submit_order") {
                require_exact_fields(
                    value,
                    {"action", "price", "quantity", "side"});
                const std::string side_value = required_string(value, "side");
                Side side;
                if (side_value == "buy") {
                    side = Side::Buy;
                } else if (side_value == "sell") {
                    side = Side::Sell;
                } else {
                    throw DeepSeekDecisionError(
                        "DeepSeek action has an invalid side");
                }
                return SubmitOrderAction{
                    side,
                    required_positive_int64(value, "price"),
                    required_positive_int64(value, "quantity"),
                };
            }
            if (action == "propose_contract") {
                require_exact_fields(
                    value,
                    {"action",
                     "counterparty",
                     "payee",
                     "payer",
                     "payment_amount",
                     "resource",
                     "resource_quantity"});
                const std::string resource = required_string(
                    value,
                    "resource");
                if (resource != "compute_credit") {
                    throw DeepSeekDecisionError(
                        "DeepSeek action has an invalid resource");
                }
                return ProposeContractAction{
                    required_positive_uint64(value, "counterparty"),
                    ContractTerms{
                        required_positive_uint64(value, "payer"),
                        required_positive_uint64(value, "payee"),
                        required_positive_int64(value, "payment_amount"),
                        ResourceKind::ComputeCredit,
                        required_positive_int64(
                            value,
                            "resource_quantity")}};
            }
            if (action == "accept_contract") {
                require_exact_fields(value, {"action", "contract_id"});
                return AcceptContractAction{required_contract_id(value)};
            }
            if (action == "reject_contract") {
                require_exact_fields(value, {"action", "contract_id"});
                return RejectContractAction{required_contract_id(value)};
            }
            if (action == "fulfill_resource_obligation") {
                require_exact_fields(value, {"action", "contract_id"});
                return FulfillResourceObligationAction{
                    required_contract_id(value)};
            }
            if (action == "settle_payment_obligation") {
                require_exact_fields(value, {"action", "contract_id"});
                return SettlePaymentObligationAction{
                    required_contract_id(value)};
            }
            throw DeepSeekDecisionError(
                "DeepSeek action has an unknown action");
        } catch (const DeepSeekDecisionError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw DeepSeekDecisionError(
                "DeepSeek action is not valid strict JSON");
        }
    }

    DeepSeekDecisionProvider::DeepSeekDecisionProvider(
        CompletionFunction complete)
        : complete_(std::move(complete)) {
        if (!complete_) {
            throw std::invalid_argument(
                "DeepSeek completion function must be provided");
        }
    }

    AgentAction DeepSeekDecisionProvider::decide(
        const AgentObservation& observation) const {
        return parse_deepseek_action(
            complete_(build_deepseek_prompt(observation)));
    }
}  // namespace exchange
