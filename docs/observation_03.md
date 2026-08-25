# Performance Observation 03

> 快速结论：Experiment 03 清理了 cancellation API，但性能结果是 neutral；Experiment 04 对 `order_index_` 做 peak-based pre-sizing，Core median throughput 提升 8.71%，E2E 提升 7.13%。

## 起点

Experiment 02 之后，`std::unordered_map::find` 仍然是 E2E 的主要 hotspot。沿着 cancellation path 看，发现一次撤单做了两次 `order_index_` lookup：

```text
MatchingEngine::cancel_order
-> OrderBook::find_order
-> OrderBook::cancel_order
-> order_index_.find
```

第一次 lookup 是为了拿到 `OrderCancelled` event 需要的 Order snapshot，第二次才是真正执行撤单。

## Experiment 03：Cancellation Execution Result

### 要验证什么

如果 OrderBook 用执行撤单的同一次 lookup 返回 cancelled Order，E2E cancellation path 就能少查一次 hash table。实验目标是确认这个重复 lookup 是否有 measurable cost。

### 实际改动

OrderBook 增加：

```cpp
std::optional<Order> cancel_order_with_result(OrderId order_id);
```

原有 `bool cancel_order(OrderId)` 保留，两个 public API 共用唯一的 internal cancellation implementation。

OrderBook 仍然是 order state 的 owner。成功撤单返回 cancellation 时的 remaining Order，并发布一个 `OrderCancelled`；失败撤单返回 failure、不发 Event、也不改变状态。

### 结果

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 170.803 ms | 175.026 ms | +2.47% |
| Commands/s | 5.85473M | 5.71347M | -2.41% |
| Adds/s | 5.26881M | 5.14169M | -2.41% |
| Trades/s | 4.08033M | 3.98188M | -2.41% |
| Elapsed-time CV | 3.93% | 3.20% | -0.73 pp |
| Throughput CV | 3.88% | 3.23% | -0.66 pp |

观察到的 -2.41% 小于 benchmark 自身的 CV，因此不能说它造成了 material regression，也没有证据证明它提升了性能。这个 Experiment 的性能结论是 neutral。

### 验证方式

44/44 tests passed。Disassembly 和 After profile 确认调用路径已经变成：

```text
MatchingEngine::cancel_order
-> OrderBook::cancel_order_with_result
-> OrderBook::cancel_order_impl
-> one order_index_.find
```

成功和失败路径都没有 preceding `OrderBook::find_order()`。Profile 中残留的 0.04% `find_order` samples 来自 timed section 外的 workload reconciliation。

虽然性能没有明显变化，但新 API 去掉了重复查询，也更准确地表达了 cancellation result，所以没有 revert 的 correctness 或 architecture 理由。

## Experiment 04：Order Index Pre-sizing

### 为什么接着做这个

Fresh profile 里，`unordered_map::_M_rehash` 在 Core 和 E2E 中仍占大约 4%。`find`、`emplace` 和 `erase` 也都很明显。

这里暂不替换 container，也不调整 allocator。实验只回答一个问题：如果提前给 `order_index_` 足够的 bucket capacity，避免 timed execution 中的 growth/rehash，结果会怎样？

### Reserve capacity 怎么确定

没有直接用 1,000,000 `command_count`，因为大部分历史订单并不会同时留在 OrderBook 中。那样会严重 over-reserve，并额外改变 memory footprint 和 cache behavior。

现有 benchmark dry run 本来就会顺序执行相同 commands，因此在每条 command 完成后记录 `order_count()`，得到 deterministic `peak_active_order_count`。它是这组 workload 同时存在的 active resting orders 的精确 peak，也是合理的最小 reserve target。

### 实际改动

- `OrderBook::reserve_order_capacity(std::size_t)` 直接调用 `order_index_.reserve()`。
- `MatchingEngine` 提供一个很薄的 forwarding method，E2E 不需要暴露 mutable OrderBook。
- Core 和 E2E 每次 iteration 都在 `PauseTiming()` 内创建新状态并 reserve peak capacity。
- `max_load_factor`、matching、cancellation、Event、Replay 和 workload semantics 都没有改变。

44/44 tests passed。

### Core 结果

| 1M Core | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 116.144 ms | 106.855 ms | -8.00% |
| Commands/s | 8.61008M | 9.36044M | +8.71% |
| Adds/s | 7.74842M | 8.42368M | +8.71% |
| Trades/s | 6.00061M | 6.52356M | +8.71% |
| Elapsed-time CV | 9.99% | 11.59% | +1.60 pp |
| Throughput CV | 8.65% | 9.98% | +1.33 pp |

Core median 是正向的，但 Before/After 都有较慢的 early repetitions，CV 接近甚至超过 throughput delta。所以这里的判断是：结果 positive，但 noise 较高，不能只凭这一组数据过度解读。

### End-to-End 结果

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 170.902 ms | 159.526 ms | -6.66% |
| Commands/s | 5.85133M | 6.26860M | +7.13% |
| Adds/s | 5.26575M | 5.64126M | +7.13% |
| Trades/s | 4.07796M | 4.36877M | +7.13% |
| Elapsed-time CV | 5.05% | 2.01% | -3.04 pp |
| Throughput CV | 4.81% | 2.00% | -2.82 pp |

E2E median throughput 提升 7.13%，After CV 降到约 2%。相比 Core，这组数据更稳定，更能支持 pre-sizing 带来了 measurable improvement。

### Profile 怎么验证 timed rehash 消失了

普通 After profile 中，order-index `_M_rehash` 的 self samples 从此前约 4% 降到：

- Core：0.22%
- End-to-End：0.35%

`perf` 会采样整个 process，包括 Google Benchmark 的 paused setup，所以 profile 里仍可能看到显式 `reserve()` 自己触发的 rehash。为此补充使用 DWARF call graph 区分调用来源：

| After profile | Paused `reserve()` | Workload preparation | Timed growth rehash |
|---|---:|---:|---:|
| Core | 225 samples | 32 samples | 0 |
| End-to-End | 2 samples | 49 samples | 0 |

E2E 没有 `_M_rehash` sample 来自 `ReplayEngine::replay`；Core 里所有 `_M_emplace -> _M_rehash` samples 也都属于一次性的 workload preparation。也就是说，timed `order_index_` insertion 已经不再触发 capacity-growth rehash。

## 回头看这两个 Experiment

- Experiment 03 说明：代码路径更干净，不代表 benchmark 一定会明显变快。先保留正确、清晰的 API，再诚实记录 neutral result。
- Experiment 04 说明：只有在知道真实 active-state peak 后，pre-sizing 才是一个合理实验；直接 reserve `command_count` 会混入过度分配的影响。
- `unordered_map::reserve` 改变的不只是 `_M_rehash` instruction cost，还包括 bucket allocation 时机、bucket count、load factor、collision distribution 和 cache footprint。因此这里应该说“hash-table pre-sizing 整体带来了改善”，不能把全部收益都算给 `_M_rehash`。
- 下一次继续分析前应该重新做 fresh profile，再根据新的 dominant hotspot 选择目标，而不是沿用旧 profile 猜下一步。
