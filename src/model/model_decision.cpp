#include "exchange/model/model_decision.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <string>

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
                throw ModelDecisionParseError(
                    "Model decision fields do not match its action schema");
            }
        }

        std::string required_string(const Json& value, const char* key) {
            const auto field = value.find(key);
            if (field == value.end() || !field->is_string()) {
                throw ModelDecisionParseError(
                    "Model decision requires a string field");
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
                throw ModelDecisionParseError(
                    "Model decision requires an integer field");
            }

            if (field->is_number_unsigned()) {
                const auto number = field->get<std::uint64_t>();
                if (number == 0
                    || number
                           > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max())) {
                    throw ModelDecisionParseError(
                        "Model decision integer must be positive and in range");
                }
                return static_cast<std::int64_t>(number);
            }

            const auto number = field->get<std::int64_t>();
            if (number <= 0) {
                throw ModelDecisionParseError(
                    "Model decision integer must be positive");
            }
            return number;
        }

        OrderId required_order_id(const Json& value) {
            const auto field = value.find("order_id");
            if (field == value.end()
                || (!field->is_number_integer()
                    && !field->is_number_unsigned())) {
                throw ModelDecisionParseError(
                    "Model cancel decision requires an integer order_id");
            }
            if (field->is_number_unsigned()) {
                const OrderId order_id = field->get<OrderId>();
                if (order_id == 0) {
                    throw ModelDecisionParseError(
                        "Model cancel order_id must be non-zero");
                }
                return order_id;
            }

            const auto signed_order_id = field->get<std::int64_t>();
            if (signed_order_id <= 0) {
                throw ModelDecisionParseError(
                    "Model cancel order_id must be non-zero");
            }
            return static_cast<OrderId>(signed_order_id);
        }
    }  // namespace

    ModelDecision parse_model_decision(std::string_view content) {
        try {
            const Json value = Json::parse(content);
            if (!value.is_object()) {
                throw ModelDecisionParseError(
                    "Model decision must be a JSON object");
            }

            const std::string action = required_string(value, "action");
            if (action == "hold") {
                require_exact_fields(value, {"action"});
                return ModelHoldDecision{};
            }
            if (action == "cancel_order") {
                require_exact_fields(value, {"action", "order_id"});
                return ModelCancelOrderDecision{required_order_id(value)};
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
                    throw ModelDecisionParseError(
                        "Model submit decision has an invalid side");
                }
                return ModelSubmitOrderDecision{
                    side,
                    required_positive_int64(value, "price"),
                    required_positive_int64(value, "quantity")};
            }
            throw ModelDecisionParseError(
                "Model decision has an unknown action");
        } catch (const ModelDecisionParseError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw ModelDecisionParseError(
                "Model decision is not valid strict JSON");
        }
    }
}  // namespace exchange
