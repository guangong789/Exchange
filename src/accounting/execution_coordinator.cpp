#include "exchange/accounting/execution_coordinator.hpp"

#include <exception>
#include <limits>
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

        struct ProjectedBalance {
            Balance balance{};
            bool row_exists{};
        };

        struct OrderLifecycleAction {
            OrderId order_id{};
            AccountId account_id{};
            AssetId asset_id{};
            Amount remaining_amount{};
            Side side{};
            bool fully_filled{};
        };

        struct FinancialPreflightPlan {
            std::vector<ExpectedExecution> expected_executions;
            std::vector<ConsumptionAction> consumption_actions;
            std::map<OrderId, ProjectedOrderState> projected_orders;
            std::vector<OrderLifecycleAction> lifecycle_actions;
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
            plan.lifecycle_actions.reserve(previews.size() * 2);

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
            std::map<std::pair<AccountId, AssetId>, ProjectedBalance>
                projected_balances;
            std::map<OrderId, Quantity> projected_execution_quantity;
            std::vector<OrderId> involved_order_ids;
            involved_order_ids.reserve(previews.size() * 2);

            const auto load_projected_balance = [&](AccountId account_id,
                                                    AssetId asset_id)
                -> ProjectedBalance& {
                const auto key = std::pair{account_id, asset_id};
                const auto existing = projected_balances.find(key);
                if (existing != projected_balances.end()) {
                    return existing->second;
                }

                if (!accounts.contains_account(account_id)) {
                    throw std::logic_error(
                        "reservation owner account does not exist");
                }

                const auto snapshot = accounts.find_balance(
                    account_id,
                    asset_id);
                if (snapshot.has_value()
                    && (snapshot->available < 0 || snapshot->reserved < 0)) {
                    throw std::logic_error(
                        "account balance contains a negative amount");
                }
                return projected_balances
                    .emplace(
                        key,
                        snapshot.has_value()
                            ? ProjectedBalance{*snapshot, true}
                            : ProjectedBalance{Balance{}, false})
                    .first->second;
            };

            const auto project_credit = [&](AccountId account_id,
                                            AssetId asset_id,
                                            Amount amount) {
                if (amount <= 0) {
                    throw std::logic_error(
                        "projected credit must be positive");
                }

                ProjectedBalance& projected = load_projected_balance(
                    account_id,
                    asset_id);
                if (amount
                    > std::numeric_limits<Amount>::max()
                          - projected.balance.available) {
                    throw std::overflow_error(
                        "projected available credit overflow");
                }

                projected.balance.available += amount;
                projected.row_exists = true;
            };

            const auto apply_action = [&](OrderId order_id,
                                          Side side,
                                          Amount amount)
                -> OrderReservation {
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
                if (side_inserted) {
                    involved_order_ids.push_back(order_id);
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

                ProjectedBalance& projected = load_projected_balance(
                    reservation->account_id,
                    asset_id);
                if (!projected.row_exists) {
                    throw std::logic_error(
                        "reserved account balance is missing");
                }
                if (projected.balance.reserved < amount) {
                    throw std::logic_error(
                        "projected consumption exceeds account reserved balance");
                }
                projected.balance.reserved -= amount;

                plan.consumption_actions.push_back(ConsumptionAction{
                    order_id,
                    reservation->account_id,
                    asset_id,
                    amount,
                    side,
                });
                return *reservation;
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
                const OrderReservation buy_record = apply_action(
                    execution.buy_order_id,
                    Side::Buy,
                    amounts.quote_amount);
                const OrderReservation sell_record = apply_action(
                    execution.sell_order_id,
                    Side::Sell,
                    amounts.base_amount);
                project_credit(
                    buy_record.account_id,
                    instrument.base_asset,
                    amounts.base_amount);
                project_credit(
                    sell_record.account_id,
                    instrument.quote_asset,
                    amounts.quote_amount);

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

            for (const OrderId order_id : involved_order_ids) {
                const auto reservation = reservations.find(order_id);
                if (!reservation.has_value()) {
                    throw std::logic_error(
                        "projected order reservation disappeared");
                }
                const ProjectedOrderState& state =
                    plan.projected_orders.at(order_id);
                plan.lifecycle_actions.push_back(OrderLifecycleAction{
                    order_id,
                    reservation->account_id,
                    reservation->asset_id,
                    state.remaining_amount,
                    projected_sides.at(order_id),
                    state.fully_filled,
                });
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

        void clear_actual_trades(
            const InstrumentContext& instrument,
            const std::vector<Trade>& actual_trades,
            const FinancialPreflightPlan& plan,
            AccountStore& accounts,
            OrderReservationStore& reservations,
            const OrderBook& order_book) {
            if (plan.consumption_actions.size()
                != actual_trades.size() * 2) {
                throw std::logic_error(
                    "actual Trades do not match planned consumption count");
            }

            for (std::size_t index = 0;
                 index < actual_trades.size();
                 ++index) {
                const Trade& trade = actual_trades[index];
                const ConsumptionAction& buy_action =
                    plan.consumption_actions[index * 2];
                const ConsumptionAction& sell_action =
                    plan.consumption_actions[index * 2 + 1];
                const TradeFinancialAmounts amounts = calculate_trade_amounts(
                    instrument,
                    trade.price,
                    trade.quantity);
                if (buy_action.order_id != trade.buy_order_id
                    || buy_action.side != Side::Buy
                    || buy_action.asset_id != instrument.quote_asset
                    || buy_action.amount != amounts.quote_amount
                    || sell_action.order_id != trade.sell_order_id
                    || sell_action.side != Side::Sell
                    || sell_action.asset_id != instrument.base_asset
                    || sell_action.amount != amounts.base_amount) {
                    throw std::logic_error(
                        "actual Trade financial facts do not match preflight");
                }
                accounts.consume_reserved(
                    buy_action.account_id,
                    instrument.quote_asset,
                    amounts.quote_amount);
                reservations.consume(
                    trade.buy_order_id,
                    amounts.quote_amount);

                accounts.consume_reserved(
                    sell_action.account_id,
                    instrument.base_asset,
                    amounts.base_amount);
                reservations.consume(
                    trade.sell_order_id,
                    amounts.base_amount);

                accounts.credit_available(
                    buy_action.account_id,
                    instrument.base_asset,
                    amounts.base_amount);
                accounts.credit_available(
                    sell_action.account_id,
                    instrument.quote_asset,
                    amounts.quote_amount);
            }

            for (const OrderLifecycleAction& action :
                 plan.lifecycle_actions) {
                const bool is_live = order_book.find_order(
                    action.order_id).has_value();
                if (!action.fully_filled) {
                    if (!is_live) {
                        throw std::logic_error(
                            "partially filled order is not live");
                    }
                    continue;
                }
                if (is_live) {
                    throw std::logic_error(
                        "fully filled order remains live");
                }

                const auto record = reservations.find(action.order_id);
                if (!record.has_value()) {
                    throw std::logic_error(
                        "completed order reservation is missing");
                }
                if (record->account_id != action.account_id
                    || record->asset_id != action.asset_id
                    || record->remaining_amount != action.remaining_amount) {
                    throw std::logic_error(
                        "completed order reservation differs from preflight");
                }

                if (action.side == Side::Buy) {
                    if (record->remaining_amount > 0) {
                        accounts.release(
                            record->account_id,
                            instrument.quote_asset,
                            record->remaining_amount);
                    }
                    static_cast<void>(reservations.remove(action.order_id));
                    continue;
                }

                if (record->remaining_amount != 0) {
                    throw std::logic_error(
                        "fully filled SELL has residual reservation");
                }
                static_cast<void>(reservations.remove(action.order_id));
            }
        }

        [[nodiscard]] std::size_t residual_release_count(
            const FinancialPreflightPlan& plan) {
            std::size_t count = 0;
            for (const OrderLifecycleAction& action :
                 plan.lifecycle_actions) {
                if (action.fully_filled
                    && action.side == Side::Buy
                    && action.remaining_amount > 0) {
                    ++count;
                }
            }
            return count;
        }

        void append_actual_trade_and_release_transactions(
            const InstrumentContext& instrument,
            const std::vector<Trade>& actual_trades,
            const FinancialPreflightPlan& plan,
            std::vector<LedgerTransaction>& ledger_batch) {
            if (plan.consumption_actions.size()
                != actual_trades.size() * 2) {
                throw std::logic_error(
                    "actual Trades do not match planned ownership count");
            }

            for (std::size_t index = 0;
                 index < actual_trades.size();
                 ++index) {
                const ConsumptionAction& buy_action =
                    plan.consumption_actions[index * 2];
                const ConsumptionAction& sell_action =
                    plan.consumption_actions[index * 2 + 1];
                if (buy_action.side != Side::Buy
                    || sell_action.side != Side::Sell
                    || buy_action.order_id
                           != actual_trades[index].buy_order_id
                    || sell_action.order_id
                           != actual_trades[index].sell_order_id) {
                    throw std::logic_error(
                        "actual Trade ownership does not match preflight");
                }
                ledger_batch.push_back(make_trade_ledger_transaction(
                    instrument,
                    actual_trades[index],
                    buy_action.account_id,
                    sell_action.account_id));
            }

            for (const OrderLifecycleAction& action :
                 plan.lifecycle_actions) {
                if (!action.fully_filled
                    || action.side != Side::Buy
                    || action.remaining_amount <= 0) {
                    continue;
                }
                ledger_batch.push_back(make_release_ledger_transaction(
                    action.order_id,
                    action.account_id,
                    action.asset_id,
                    action.remaining_amount));
            }
        }
    }  // namespace

    ExecutionCoordinator::ExecutionCoordinator(
        InstrumentContext instrument,
        AccountStore& accounts,
        OrderReservationStore& reservations,
        MatchingEngine& matching_engine,
        EventCollector& events,
        Ledger& ledger)
        : instrument_(instrument),
          accounts_(accounts),
          reservations_(reservations),
          matching_engine_(matching_engine),
          events_(events),
          ledger_(ledger) {
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

        std::vector<LedgerTransaction> ledger_batch;
        try {
            const std::size_t release_count = residual_release_count(
                preflight.plan);
            if (release_count > ledger_batch.max_size() - 1
                || previews.size()
                       > ledger_batch.max_size() - 1 - release_count) {
                throw std::length_error(
                    "submit Ledger batch capacity overflow");
            }
            ledger_batch.reserve(1 + previews.size() + release_count);
            ledger_batch.push_back(make_reserve_ledger_transaction(
                request.order.id,
                request.account_id,
                requirement.asset_id,
                requirement.amount));
        } catch (...) {
            const std::exception_ptr ledger_build_failure =
                std::current_exception();
            accounts_.release(
                request.account_id,
                requirement.asset_id,
                requirement.amount);
            static_cast<void>(reservations_.remove(request.order.id));
            events_.clear();
            std::rethrow_exception(ledger_build_failure);
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
        append_actual_trade_and_release_transactions(
            instrument_,
            trades,
            preflight.plan,
            ledger_batch);
        auto prepared_ledger_batch = ledger_.prepare_batch(
            std::move(ledger_batch));
        if (!trades.empty()) {
            clear_actual_trades(
                instrument_,
                trades,
                preflight.plan,
                accounts_,
                reservations_,
                matching_engine_.order_book());
        }
        prepared_ledger_batch.commit();

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

        auto prepared_ledger_batch = ledger_.prepare_batch({
            make_release_ledger_transaction(
                order_id,
                reservation->account_id,
                reservation->asset_id,
                reservation->remaining_amount),
        });

        if (!matching_engine_.cancel_order(order_id)) {
            throw std::logic_error(
                "reservation exists without a live matching order");
        }

        accounts_.release(
            reservation->account_id,
            reservation->asset_id,
            reservation->remaining_amount);
        static_cast<void>(reservations_.remove(order_id));
        prepared_ledger_batch.commit();
        return CancelResult::Cancelled;
    }

}  // namespace exchange
