# Performance Observation 01

> 快速结论：先把 baseline 和 hotspot 找出来，不急着改代码。第一轮 profile 把注意力指向了 Event vector growth 和 post-trade maker lookup。

## 这次要弄清楚什么

项目已经能稳定跑 1,000,000 commands，但“跑得慢”本身没有行动价值。首先需要回答两个问题：

1. Core matching 的时间主要花在哪里？
2. End-to-End 相比 Core 多出来的成本来自哪里？

这一步只做观察，不做 optimization。这样后面的每次修改都能对应一个真实 hotspot，而不是凭感觉改 STL container。

## 测量方法

- Workload：1,000,000 commands，50% buy ratio，10% cancel ratio
- 输入：固定 seed 的 deterministic synthetic workload
- Benchmark build：Release
- Profiling build：RelWithDebInfo
- 环境：WSL2

这组基线大致是：

- Core：约 8–9M commands/s
- End-to-End：约 3.5–4.2M commands/s

WSL2 的 absolute timing 会随环境波动，因此主要关注相同配置、相邻运行之间的 relative change。

还有一个限制：当前 WSL2 环境拿不到需要的 hardware PMU counters，因此 `cycles`、`instructions`、cache 和 branch 等 `perf stat` 数据不可用。这里主要依赖 `perf record` 的 function-level CPU sampling。

## Profile 显示了什么

Core 路径里比较明显的是：

- `OrderBook::add_order`
- `OrderBook::cancel_order`
- `OrderBook::rest_order`
- `malloc` / `_int_free`
- `std::unordered_map` 的 find、emplace、erase 和 rehash

这说明 dynamic allocation、`order_index_` 维护和 cancellation bookkeeping 都有成本。不过证据还不够支持直接替换 `std::map`、`std::list` 或 `std::unordered_map`。

End-to-End 里额外出现了：

- `MatchingEngine::add_order`
- `OrderBook::find_order`
- `std::vector<Event>::emplace_back`

顺着调用路径看，两个具体问题比较值得单独验证：

1. `EventCollector` 的 vector 在 replay 过程中反复 growth/reallocation。
2. 每笔 Trade 后，`MatchingEngine` 为生成 maker fill event 又调用了一次 `find_order(maker_id)`。

`ReplayEngine::replay` 自己的 sampled overhead 很小，所以它不是这一阶段的优化目标。

## 判断过程

这些热点没有被一次性改掉，而是被拆成几个可独立验证的 hypothesis：

1. 如果提前 reserve 精确的 Event 数量，E2E 应该变快；这可以单独验证 vector growth 的成本。
2. 如果 OrderBook 在 execution result 中直接带回 maker state，就可以单独验证 post-trade lookup 的成本。
3. Node-based containers 和 `order_index_` 可能影响 Core scaling，但要等前两个问题处理完并重新 profile，不能提前下结论。

下一步就是一次只验证一个 hypothesis，固定 workload、保留 Before baseline，并用多次 repetition 的 median 做比较。
