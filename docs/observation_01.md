# Performance Observation 01

## 测量背景

- Workload：1,000,000 commands，50% buy ratio，10% cancel ratio
- 使用 deterministic synthetic workload
- Benchmark 使用 Release，profiling 使用 RelWithDebInfo
- 运行环境：WSL2

基线吞吐量随运行环境有所波动：

- Core：约 8–9M commands/s
- End-to-End：约 3.5–4.2M commands/s

WSL2 下的绝对数值依赖环境，因此主要关注相同配置下的相对变化。

## Profiling 限制

当前 WSL2 环境无法提供所需的 hardware PMU counters，因此 `cycles`、`instructions`、cache 和 branch 相关的 `perf stat` 数据不可用。本次观察基于 `perf record` 的 function-level CPU sampling。

## Profiling 发现

Core 路径的主要 hotspot 包括：

- `OrderBook::add_order`
- `OrderBook::cancel_order`
- `OrderBook::rest_order`
- `malloc` / `_int_free`
- `std::unordered_map` 的 rehash、emplace 和 erase

这表明 dynamic allocation、订单索引维护及撤单 bookkeeping 在 Core 成本中占有可测量比例，但尚不足以支持更换现有容器。

End-to-End 路径还出现：

- `MatchingEngine::add_order`
- `OrderBook::find_order`
- `std::unordered_map::find`
- `std::vector<Event>::emplace_back`

`ReplayEngine::replay` 本身的 sampled overhead 较小。每笔 Trade 后用于生成 maker fill event 的 `find_order(maker_id)` 会额外访问 `order_index_`；`EventCollector` 的 event vector 也会在 replay 期间反复增长。

## 待验证 Hypotheses

1. `EventCollector` 的重复扩容和 allocation 可能贡献了显著的 End-to-End overhead。
2. 每笔 Trade 后的 maker-state lookup 可能贡献了显著的 End-to-End overhead。
3. Node-based containers 与 `order_index_` 维护可能影响大 workload 下的 Core scaling，但需要独立实验验证。
