# Performance Observation 02

## 前序发现

- `EventCollector` event vector 的重复增长是可测量的 End-to-End hotspot。
- 完成 capacity reserve 并重新 profiling 后，通过 `OrderBook::find_order` 获取 post-trade maker state 成为 `MatchingEngine::add_order` 的主要 hotspot。

## Optimization 1：Event Capacity Reservation

Benchmark setup 在计时区外为 `EventCollector` reserve 精确的 event capacity，事件生成和存储语义保持不变。

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 229 ms | 169 ms | -26.20% |
| Commands/s | 4.37591M | 5.93003M | +35.52% |

## Optimization 2：Maker Execution State

OrderBook 在 execution result 中直接返回 maker state，取代 post-trade maker lookup；`Trade` 和 `Event` layout 保持不变。

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 209 ms | 181 ms | -13.40% |
| Commands/s | 4.78518M | 5.51748M | +15.30% |

修改后 41/41 tests passed。

## Profiling 验证

Post-Experiment-02 profile 确认，`MatchingEngine::add_order` 不再在每笔 Trade 后调用 `OrderBook::find_order`。剩余的 `find_order` samples 来自 `MatchingEngine::cancel_order` 撤单路径。

## 方法与当前状态

- 每次 optimization 只改变一个目标变量。
- Before/After 使用相同 deterministic workload 和 Release 配置。
- 每组执行 10 repetitions，以 median 作为主要比较值。
- WSL2 absolute timing 依赖环境，因此相对变化是主要证据。
