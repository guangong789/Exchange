#include "exchange/execution_coordinator.hpp"

#include <exception>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange {
    namespace {
        struct ExpectedExecution {
            OrderId buy_order_id{};
            OrderId sell_order_id{};
            Price price{};
            Quantity quantity{};
        };

        struct ConsumptionAction {
            OrderId order_id{};
            AccountId account_id{};
            AssetId asset_id{};
            Amount amount{};
            Side side{};
        };

        struct ProjectedOrderState {
            Amount remaining_amount{};
            bool fully_filled{};
        };

        struct FinancialPreflightPlan {
            std::vector<ExpectedExecution> expected_executions;
            std::vector<ConsumptionAction> consumption_actions;
            std::map<OrderId, ProjectedOrderState> projected_orders;
        };

        enum class FinancialPreflightStatus {
            Ready,
            CounterpartyNotAccountBacked,
        };

        struct FinancialPreflightResult {
            FinancialPreflightStatus status{FinancialPreflightStatus::Ready};
            FinancialPreflightPlan plan;
        };

        [[nodiscard]] Side opposite_side(Side side) {
            return side == Side::Buy ? Side::Sell : Side::Buy;
        }

        [[nodiscard]] AssetId expected_asset(
            const InstrumentContext& instrument,
            Side side) {
            return side == Side::Buy
                       ? instrument.quote_asset
                       : instrument.base_asset;
        }

        [[nodiscard]] FinancialPreflightResult build_financial_preflight(
            const InstrumentContext& instrument,
            const Order& incoming,
            const std::vector<MatchPreview>& previews,
            const AccountStore& accounts,
            const OrderReservationStore& reservations,
            const OrderBook& order_book) {
            FinancialPreflightResult result;
            auto& plan = result.plan;
            plan.expected_executions.reserve(previews.size());
            plan.consumption_actions.reserve(previews.size() * 2);

            const auto incoming_reservation = reservations.find(incoming.id);
            if (!incoming_reservation.has_value()) {
                throw std::logic_error(
                    "incoming reservation is missing during preflight");
            }

            for (const MatchPreview& preview : previews) {
                if (!reservations.find(preview.maker_order_id).has_value()) {
                    result.status =
                        FinancialPreflightStatus::CounterpartyNotAccountBacked;
                    return result;
                }
            }

            std::map<OrderId, Side> projected_sides;
            std::map<OrderId, Amount> projected_remaining;
            std::map<std::pair<AccountId, AssetId>, Amount>
                projected_account_reserved;
            std::map<OrderId, Quantity> projected_execution_quantity;

            const auto apply_action = [&](OrderId order_id,
                                          Side side,
                                          Amount amount) {
                if (amount <= 0) {
                    throw std::logic_error(
                        "projected consumption must be positive");
                }

                const auto [side_it, side_inserted] =
                    projected_sides.try_emplace(order_id, side);
                if (!side_inserted && side_it->second != side) {
                    throw std::logic_error(
                        "one order appears on both trade sides");
                }

                const auto reservation = reservations.find(order_id);
                if (!reservation.has_value()) {
                    throw std::logic_error(
                        "order reservation disappeared during preflight");
                }

                const AssetId asset_id = expected_asset(instrument, side);
                if (reservation->asset_id != asset_id) {
                    throw std::logic_error(
                        "order reservation asset does not match trade side");
                }

                const auto [remaining_it, remaining_inserted] =
                    projected_remaining.try_emplace(
                        order_id,
                        reservation->remaining_amount);
                static_cast<void>(remaining_inserted);
                if (remaining_it->second < amount) {
                    throw std::logic_error(
                        "projected consumption exceeds order reservation");
                }
                remaining_it->second -= amount;

                const auto account_key =
                    std::pair{reservation->account_id, asset_id};
                auto account_it = projected_account_reserved.find(account_key);
                if (account_it == projected_account_reserved.end()) {
                    const auto balance = accounts.find_balance(
                        reservation->account_id,
                        asset_id);
                    if (!balance.has_value()) {
                        throw std::logic_error(
                            "reserved account balance is missing");
                    }
                    account_it = projected_account_reserved
                                     .emplace(account_key, balance->reserved)
                                     .first;
                }
                if (account_it->second < amount) {
                    throw std::logic_error(
                        "projected consumption exceeds account reserved balance");
                }
                account_it->second -= amount;

                plan.consumption_actions.push_back(ConsumptionAction{
                    order_id,
                    reservation->account_id,
                    asset_id,
                    amount,
                    side,
                });
            };

            for (const MatchPreview& preview : previews) {
                const Side maker_side = opposite_side(incoming.side);
                const ExpectedExecution execution =
                    incoming.side == Side::Buy
                        ? ExpectedExecution{
                              incoming.id,
                              preview.maker_order_id,
                              preview.execution_price,
                              preview.execution_quantity}
                        : ExpectedExecution{
                              preview.maker_order_id,
                              incoming.id,
                              preview.execution_price,
                              preview.execution_quantity};
                plan.expected_executions.push_back(execution);

                const TradeFinancialAmounts amounts = calculate_trade_amounts(
                    instrument,
                    preview.execution_price,
                    preview.execution_quantity);
                apply_action(
                    execution.buy_order_id,
                    Side::Buy,
                    amounts.quote_amount);
                apply_action(
                    execution.sell_order_id,
                    Side::Sell,
                    amounts.base_amount);

                projected_execution_quantity[incoming.id] +=
                    preview.execution_quantity;
                projected_execution_quantity[preview.maker_order_id] +=
                    preview.execution_quantity;

                const auto maker = order_book.find_order(
                    preview.maker_order_id);
                if (!maker.has_value() || maker->side != maker_side) {
                    throw std::logic_error(
                        "previewed maker state is inconsistent");
                }
                if (projected_execution_quantity[preview.maker_order_id]
                    > maker->quantity) {
                    throw std::logic_error(
                        "projected maker execution exceeds live quantity");
                }
            }

            if (projected_execution_quantity[incoming.id]
                > incoming.quantity) {
                throw std::logic_error(
                    "projected taker execution exceeds incoming quantity");
            }

            for (const auto& [order_id, remaining_amount] :
                 projected_remaining) {
                const bool is_incoming = order_id == incoming.id;
                const auto live_order = is_incoming
                                            ? std::optional<Order>{incoming}
                                            : order_book.find_order(order_id);
                if (!live_order.has_value()) {
                    throw std::logic_error(
                        "projected order has no quantity snapshot");
                }

                const bool fully_filled =
                    projected_execution_quantity[order_id]
                    == live_order->quantity;
                if (!fully_filled && remaining_amount <= 0) {
                    throw std::logic_error(
                        "partially filled order has no projected reservation");
                }
                if (fully_filled
                    && projected_sides.at(order_id) == Side::Sell
                    && remaining_amount != 0) {
                    throw std::logic_error(
                        "fully filled sell has residual reservation");
                }

                plan.projected_orders.emplace(
                    order_id,
                    ProjectedOrderState{remaining_amount, fully_filled});
            }

            return result;
        }

        void verify_actual_executions(
            const FinancialPreflightPlan& plan,
            const std::vector<Trade>& actual_trades) {
            if (actual_trades.size() != plan.expected_executions.size()) {
                throw std::logic_error(
                    "actual Trade count does not match preview");
            }

            for (std::size_t index = 0; index < actual_trades.size(); ++index) {
                const Trade& actual = actual_trades[index];
                const ExpectedExecution& expected =
                    plan.expected_executions[index];
                if (actual.buy_order_id != expected.buy_order_id
                    || actual.sell_order_id != expected.sell_order_id
                    || actual.price != expected.price
                    || actual.quantity != expected.quantity) {
                    throw std::logic_error(
                        "actual Trade sequence does not match preview");
                }
            }
        }

        void consume_actual_trades(
            const InstrumentContext& instrument,
            const std::vector<Trade>& actual_trades,
            AccountStore& accounts,
            OrderReservationStore& reservations,
            const OrderBook& order_book) {
            std::vector<OrderId> involved_order_ids;
            involved_order_ids.reserve(actual_trades.size() * 2);
            std::map<OrderId, Side> involved_sides;

            const auto track_order = [&](OrderId order_id, Side side) {
                const auto [side_it, inserted] =
                    involved_sides.try_emplace(order_id, side);
                if (!inserted && side_it->second != side) {
                    throw std::logic_error(
                        "one actual order appears on both trade sides");
                }
                if (inserted) {
                    involved_order_ids.push_back(order_id);
                }
            };

            for (const Trade& trade : actual_trades) {
                track_order(trade.buy_order_id, Side::Buy);
                track_order(trade.sell_order_id, Side::Sell);
            }

            for (const Trade& trade : actual_trades) {
                const TradeFinancialAmounts amounts = calculate_trade_amounts(
                    instrument,
                    trade.price,
                    trade.quantity);

                const auto buy_record = reservations.find(trade.buy_order_id);
                if (!buy_record.has_value()) {
                    throw std::logic_error(
                        "actual BUY reservation is missing");
                }
                if (buy_record->asset_id != instrument.quote_asset) {
                    throw std::logic_error(
                        "actual BUY reservation asset is inconsistent");
                }
                accounts.consume_reserved(
                    buy_record->account_id,
                    instrument.quote_asset,
                    amounts.quote_amount);
                reservations.consume(
                    trade.buy_order_id,
                    amounts.quote_amount);

                const auto sell_record = reservations.find(
                    trade.sell_order_id);
                if (!sell_record.has_value()) {
                    throw std::logic_error(
                        "actual SELL reservation is missing");
                }
                if (sell_record->asset_id != instrument.base_asset) {
                    throw std::logic_error(
                        "actual SELL reservation asset is inconsistent");
                }
                accounts.consume_reserved(
                    sell_record->account_id,
                    instrument.base_asset,
                    amounts.base_amount);
                reservations.consume(
                    trade.sell_order_id,
                    amounts.base_amount);
            }

            for (const OrderId order_id : involved_order_ids) {
                if (order_book.find_order(order_id).has_value()) {
                    continue;
                }

                const auto record = reservations.find(order_id);
                if (!record.has_value()) {
                    throw std::logic_error(
                        "completed order reservation is missing");
                }

                if (involved_sides.at(order_id) == Side::Buy) {
                    if (record->remaining_amount > 0) {
                        accounts.release(
                            record->account_id,
                            instrument.quote_asset,
                            record->remaining_amount);
                    }
                    static_cast<void>(reservations.remove(order_id));
                    continue;
                }

                if (record->remaining_amount != 0) {
                    throw std::logic_error(
                        "fully filled SELL has residual reservation");
                }
                static_cast<void>(reservations.remove(order_id));
            }
        }
    }  // namespace

    ExecutionCoordinator::ExecutionCoordinator(
        InstrumentContext instrument,
        AccountStore& accounts,
        OrderReservationStore& reservations,
        MatchingEngine& matching_engine,
        EventCollector& events)
        : instrument_(instrument),
          accounts_(accounts),
          reservations_(reservations),
          matching_engine_(matching_engine),
          events_(events) {
        validate_instrument_context(instrument_);
    }

    SubmitResult ExecutionCoordinator::submit_order(
        const OrderAdmissionRequest& request) {
        events_.clear();

        if (request.account_id == 0) {
            throw std::invalid_argument("account ID must be non-zero");
        }
        if (request.order.id == 0) {
            return SubmitResult::InvalidOrder;
        }

        const ReservationRequirement requirement =
            calculate_order_reservation(
                instrument_,
                request.order.side,
                request.order.price,
                request.order.quantity);

        if (accounts_.reserve(
                request.account_id,
                requirement.asset_id,
                requirement.amount)
            == ReserveResult::InsufficientFunds) {
            return SubmitResult::InsufficientFunds;
        }

        bool created = false;
        try {
            created = reservations_.create(
                request.order.id,
                request.account_id,
                requirement.asset_id,
                requirement.amount);
        } catch (...) {
            const std::exception_ptr insertion_failure =
                std::current_exception();
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            std::rethrow_exception(insertion_failure);
        }

        if (!created) {
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            return SubmitResult::DuplicateOrder;
        }

        std::vector<MatchPreview> previews;
        try {
            previews = matching_engine_.order_book().preview_matches(
                request.order);
        } catch (const std::invalid_argument&) {
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            static_cast<void>(reservations_.remove(request.order.id));
            events_.clear();
            return SubmitResult::InvalidOrder;
        }

        FinancialPreflightResult preflight;
        try {
            preflight = build_financial_preflight(
                instrument_,
                request.order,
                previews,
                accounts_,
                reservations_,
                matching_engine_.order_book());
        } catch (...) {
            const std::exception_ptr preflight_failure =
                std::current_exception();
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            static_cast<void>(reservations_.remove(request.order.id));
            events_.clear();
            std::rethrow_exception(preflight_failure);
        }

        if (preflight.status
            == FinancialPreflightStatus::CounterpartyNotAccountBacked) {
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            static_cast<void>(reservations_.remove(request.order.id));
            events_.clear();
            return SubmitResult::CounterpartyNotAccountBacked;
        }

        std::vector<Trade> trades;
        try {
            trades = matching_engine_.add_order(request.order);
        } catch (const std::invalid_argument&) {
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            static_cast<void>(reservations_.remove(request.order.id));
            events_.clear();
            return SubmitResult::InvalidOrder;
        }

        verify_actual_executions(preflight.plan, trades);
        if (!trades.empty()) {
            consume_actual_trades(
                instrument_,
                trades,
                accounts_,
                reservations_,
                matching_engine_.order_book());
        }

        return SubmitResult::Accepted;
    }

    CancelResult ExecutionCoordinator::cancel_order(
        AccountId requester,
        OrderId order_id) {
        events_.clear();

        if (requester == 0) {
            throw std::invalid_argument("requester account ID must be non-zero");
        }
        if (order_id == 0) {
            throw std::invalid_argument("order ID must be non-zero");
        }

        const auto reservation = reservations_.find(order_id);
        if (!reservation.has_value()) {
            return CancelResult::NotFound;
        }
        if (reservation->account_id != requester) {
            return CancelResult::NotOwner;
        }
        if (reservation->remaining_amount == 0) {
            throw std::logic_error(
                "cannot cancel a zero-remaining reservation");
        }

        if (!matching_engine_.cancel_order(order_id)) {
            throw std::logic_error(
                "reservation exists without a live matching order");
        }

        accounts_.release(
            reservation->account_id,
            reservation->asset_id,
            reservation->remaining_amount);
        static_cast<void>(reservations_.remove(order_id));
        return CancelResult::Cancelled;
    }

}  // namespace exchange
