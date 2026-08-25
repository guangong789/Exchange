#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

#include "exchange/command.hpp"
#include "exchange/event_collector.hpp"
#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"
#include "exchange/replay_engine.hpp"
#include "exchange/workload_generator.hpp"

namespace exchange {
    namespace {
        constexpr std::size_t kSmallWorkload = 10'000;
        constexpr std::size_t kMediumWorkload = 100'000;
        constexpr std::size_t kLargeWorkload = 1'000'000;

        struct WorkloadBaseline {
            std::vector<Command> commands;
            std::size_t add_count{};
            std::size_t trade_count{};
            std::size_t event_count{};
            std::size_t peak_active_order_count{};
        };

        WorkloadConfig workload_config(std::size_t command_count) {
            return WorkloadConfig{
                .command_count = command_count,
                .buy_ratio_bps = 5'000,
                .cancel_ratio_bps = 1'000,
                .base_price = 100'000,
                .price_variation = 500,
                .min_quantity = 1,
                .max_quantity = 100,
                .seed = 0x5EED,
            };
        }

        WorkloadBaseline make_workload(std::size_t command_count) {
            const SyntheticWorkloadGenerator generator;
            WorkloadBaseline workload{
                .commands = generator.generate(workload_config(command_count)),
            };

            OrderBook order_book;
            for (const Command& command : workload.commands) {
                if (const auto* add_order =
                        std::get_if<AddOrder>(&command.payload)) {
                    ++workload.add_count;
                    workload.trade_count +=
                        order_book.add_order(add_order->order).size();
                } else {
                    const auto& cancel_order =
                        std::get<CancelOrder>(command.payload);
                    if (!order_book.cancel_order(cancel_order.order_id)) {
                        throw std::logic_error(
                            "generated benchmark cancellation target is not active");
                    }
                }

                workload.peak_active_order_count = std::max(
                    workload.peak_active_order_count,
                    order_book.order_count());
            }

            workload.event_count =
                workload.commands.size() + 3U * workload.trade_count;

            return workload;
        }

        const WorkloadBaseline& get_workload(std::size_t command_count) {
            switch (command_count) {
                case kSmallWorkload: {
                    static const WorkloadBaseline workload =
                        make_workload(kSmallWorkload);
                    return workload;
                }
                case kMediumWorkload: {
                    static const WorkloadBaseline workload =
                        make_workload(kMediumWorkload);
                    return workload;
                }
                case kLargeWorkload: {
                    static const WorkloadBaseline workload =
                        make_workload(kLargeWorkload);
                    return workload;
                }
                default:
                    throw std::invalid_argument("unsupported benchmark workload size");
            }
        }

        void set_rate_counters(benchmark::State& state,
                            const WorkloadBaseline& workload) {
            const auto rate = benchmark::Counter::kIsIterationInvariantRate;
            state.counters["commands/s"] = benchmark::Counter(
                static_cast<double>(workload.commands.size()), rate);
            state.counters["adds/s"] = benchmark::Counter(
                static_cast<double>(workload.add_count), rate);
            state.counters["trades/s"] = benchmark::Counter(
                static_cast<double>(workload.trade_count), rate);
        }

        void BM_OrderBookCore(benchmark::State& state) {
            const auto command_count = static_cast<std::size_t>(state.range(0));
            const WorkloadBaseline& workload = get_workload(command_count);
            std::optional<OrderBook> order_book;

            for (auto _ : state) {
                static_cast<void>(_);
                state.PauseTiming();
                order_book.emplace();
                order_book->reserve_order_capacity(
                    workload.peak_active_order_count);
                state.ResumeTiming();

                for (const Command& command : workload.commands) {
                    if (const auto* add_order =
                            std::get_if<AddOrder>(&command.payload)) {
                        static_cast<void>(order_book->add_order(add_order->order));
                    } else {
                        const auto& cancel_order =
                            std::get<CancelOrder>(command.payload);
                        static_cast<void>(
                            order_book->cancel_order(cancel_order.order_id));
                    }
                }
                benchmark::DoNotOptimize(order_book->order_count());

                state.PauseTiming();
                order_book.reset();
                state.ResumeTiming();
            }

            set_rate_counters(state, workload);
        }

        void BM_EndToEndReplay(benchmark::State& state) {
            const auto command_count = static_cast<std::size_t>(state.range(0));
            const WorkloadBaseline& workload = get_workload(command_count);
            std::optional<EventCollector> event_collector;
            std::optional<MatchingEngine> matching_engine;
            std::optional<ReplayEngine> replay_engine;

            for (auto _ : state) {
                static_cast<void>(_);
                state.PauseTiming();
                event_collector.emplace();
                event_collector->reserve(workload.event_count);
                matching_engine.emplace(*event_collector);
                matching_engine->reserve_order_capacity(
                    workload.peak_active_order_count);
                replay_engine.emplace(*matching_engine);
                state.ResumeTiming();

                replay_engine->replay(workload.commands);
                benchmark::DoNotOptimize(event_collector->size());

                state.PauseTiming();
                replay_engine.reset();
                matching_engine.reset();
                event_collector.reset();
                state.ResumeTiming();
            }

            set_rate_counters(state, workload);
        }

        BENCHMARK(BM_OrderBookCore)
            ->ArgName("commands")
            ->Arg(static_cast<std::int64_t>(kSmallWorkload))
            ->Arg(static_cast<std::int64_t>(kMediumWorkload))
            ->Arg(static_cast<std::int64_t>(kLargeWorkload))
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);

        BENCHMARK(BM_EndToEndReplay)
            ->ArgName("commands")
            ->Arg(static_cast<std::int64_t>(kSmallWorkload))
            ->Arg(static_cast<std::int64_t>(kMediumWorkload))
            ->Arg(static_cast<std::int64_t>(kLargeWorkload))
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
    }
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
