#include "exchange/replay/workload_generator.hpp"

#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "exchange/matching/event_collector.hpp"
#include "exchange/matching/matching_engine.hpp"

namespace exchange {
    namespace {
        constexpr std::uint64_t kBasisPoints = 10'000;

        class ActiveOrderIds {
        public:
            [[nodiscard]] bool empty() const noexcept {
                return ids_.empty();
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return ids_.size();
            }

            [[nodiscard]] OrderId at(std::size_t index) const {
                return ids_.at(index);
            }

            void insert(OrderId order_id) {
                if (indices_.contains(order_id)) {
                    return;
                }

                indices_.emplace(order_id, ids_.size());
                ids_.push_back(order_id);
            }

            void erase(OrderId order_id) {
                const auto found = indices_.find(order_id);
                if (found == indices_.end()) {
                    return;
                }

                const std::size_t index = found->second;
                const OrderId last_id = ids_.back();
                ids_[index] = last_id;
                indices_[last_id] = index;
                ids_.pop_back();
                indices_.erase(found);
            }

        private:
            std::vector<OrderId> ids_;
            std::unordered_map<OrderId, std::size_t> indices_;
        };

        void validate_config(const WorkloadConfig& config) {
            if (config.buy_ratio_bps > kBasisPoints) {
                throw std::invalid_argument("buy ratio must be at most 10000 bps");
            }
            if (config.cancel_ratio_bps > kBasisPoints) {
                throw std::invalid_argument("cancel ratio must be at most 10000 bps");
            }
            if (config.base_price <= 0) {
                throw std::invalid_argument("base price must be positive");
            }
            if (config.price_variation < 0) {
                throw std::invalid_argument("price variation must be non-negative");
            }
            if (config.price_variation >= config.base_price) {
                throw std::invalid_argument("price range must remain positive");
            }
            if (config.price_variation >
                std::numeric_limits<Price>::max() - config.base_price) {
                throw std::invalid_argument("price range exceeds Price limits");
            }
            if (config.min_quantity <= 0 || config.max_quantity <= 0) {
                throw std::invalid_argument("quantity bounds must be positive");
            }
            if (config.min_quantity > config.max_quantity) {
                throw std::invalid_argument(
                    "minimum quantity must not exceed maximum quantity");
            }
            if (config.command_count >
                static_cast<std::size_t>(std::numeric_limits<Timestamp>::max())) {
                throw std::invalid_argument("command count exceeds Timestamp limits");
            }
        }

        std::uint64_t sample_below(std::mt19937_64& random, std::uint64_t upper_exclusive) {
            const std::uint64_t rejection_threshold =
                (std::uint64_t{0} - upper_exclusive) % upper_exclusive;

            while (true) {
                const std::uint64_t value = random();
                if (value >= rejection_threshold) {
                    return value % upper_exclusive;
                }
            }
        }

        bool sample_ratio(std::mt19937_64& random, std::uint32_t ratio_bps) {
            return sample_below(random, kBasisPoints) < ratio_bps;
        }

        Price sample_price(std::mt19937_64& random, const WorkloadConfig& config) {
            const Price minimum = config.base_price - config.price_variation;
            const std::uint64_t width =
                static_cast<std::uint64_t>(config.price_variation) * 2U + 1U;

            return minimum + static_cast<Price>(sample_below(random, width));
        }

        Quantity sample_quantity(std::mt19937_64& random, const WorkloadConfig& config) {
            const std::uint64_t width =
                static_cast<std::uint64_t>(config.max_quantity - config.min_quantity) + 1U;

            return config.min_quantity +
                static_cast<Quantity>(sample_below(random, width));
        }

        void reconcile_order(const MatchingEngine& matching_engine,
                            ActiveOrderIds& active_ids, OrderId order_id) {
            if (matching_engine.order_book().find_order(order_id).has_value()) {
                active_ids.insert(order_id);
            } else {
                active_ids.erase(order_id);
            }
        }
    }

    std::vector<Command> SyntheticWorkloadGenerator::generate(
        const WorkloadConfig& config) const {
        validate_config(config);

        std::mt19937_64 random(config.seed);
        std::vector<Command> commands;
        commands.reserve(config.command_count);

        EventCollector shadow_events;
        MatchingEngine shadow_engine(shadow_events);
        ActiveOrderIds active_ids;
        OrderId next_order_id = 1;

        for (std::size_t command_index = 0; command_index < config.command_count; ++command_index) {
            const bool generate_cancel =
                sample_ratio(random, config.cancel_ratio_bps) && !active_ids.empty();

            if (generate_cancel) {
                const std::size_t active_index = static_cast<std::size_t>(
                    sample_below(random, static_cast<std::uint64_t>(active_ids.size())));
                const OrderId order_id = active_ids.at(active_index);
                commands.push_back(Command{CommandPayload{CancelOrder{order_id}}});
                static_cast<void>(shadow_engine.cancel_order(order_id));
                active_ids.erase(order_id);
                shadow_events.clear();
                continue;
            }

            const Side side = sample_ratio(random, config.buy_ratio_bps) ? Side::Buy : Side::Sell;
            const Order order{
                next_order_id++,
                side,
                OrderType::Limit,
                sample_price(random, config),
                sample_quantity(random, config),
                static_cast<Timestamp>(command_index + 1),
            };
            commands.push_back(Command{CommandPayload{AddOrder{order}}});

            const auto trades = shadow_engine.add_order(order);
            for (const Trade& trade : trades) {
                const OrderId maker_id = side == Side::Buy ? trade.sell_order_id : trade.buy_order_id;
                reconcile_order(shadow_engine, active_ids, maker_id);
            }
            reconcile_order(shadow_engine, active_ids, order.id);
            shadow_events.clear();
        }

        return commands;
    }
}
