# Performance Observation 02

> 快速结论：前两个 isolated experiments 都有明确收益。Event capacity reservation 提升了 35.52% throughput；移除 post-trade maker lookup 又提升了 15.30%。

## 起点

Observation 01 给出了两个很具体的 hypothesis：

1. `EventCollector` 的 vector growth/reallocation 是一部分 E2E overhead。
2. 每笔 Trade 后的 maker-state lookup 是另一部分 E2E overhead。

这里的做法是一次只改变一个变量。每个 Experiment 都使用自己的 paired Before/After；因为 WSL2 timing 会波动，不应该跨 Experiment 比较绝对时间。

## Experiment 01：Event Capacity Reservation

### 为什么做

Profile 里 `std::vector<Event>::emplace_back` 很明显，而且 replay 会持续追加 Event。需要先确认其中有多少成本来自 capacity growth，而不是 Event 本身的写入。

### 实际改动

给 `EventCollector` 增加显式 `reserve()`，然后由 benchmark setup 在 timed section 外 reserve 精确的 `event_count`。

Event layout、生成顺序和存储语义都没有改变，也没有在 production logic 中自动猜 capacity。

### 结果

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 229 ms | 169 ms | -26.20% |
| Commands/s | 4.37591M | 5.93003M | +35.52% |

这说明重复 growth/reallocation 确实是 material overhead。之后的 profile 里仍然能看到 Event insertion，但那已经是正常的 Event 写入，不再是 capacity growth。

## Experiment 02：Maker Execution State

### 为什么做

Experiment 01 之后重新 profile，`MatchingEngine::add_order` 中的 post-trade maker lookup 变成了主要热点。OrderBook 本来就知道 maker 在最后一笔 Trade 后还剩多少 quantity，再查一次 `order_index_` 只是为了生成 Event。

### 实际改动

OrderBook 增加 detailed execution result，把最后一笔 Trade 对应的 maker remaining quantity 一起返回。原有 `add_order(Order)` API 和 matching implementation 保持不变，两个 API 共用同一套撮合逻辑。

`Trade`、`Event` layout 和 deterministic event order 都没有改变。

### 结果

| 1M End-to-End | Before | After | 差异 |
|---|---:|---:|---:|
| Median elapsed time | 209 ms | 181 ms | -13.40% |
| Commands/s | 4.78518M | 5.51748M | +15.30% |

修改后 41/41 tests passed。

### 验证方式

Post-Experiment-02 profile 确认：`MatchingEngine::add_order` 不再在每笔 Trade 后调用 `OrderBook::find_order`。剩余的 `find_order` samples 来自 cancellation path，而不是 matching path。

## 这两次实验确认了什么

- Profile 先提出问题，isolated experiment 再验证问题，这个顺序是有效的。
- “能在 owner 内部顺手返回的 execution state”，不应该由上层重新 lookup。
- Before/After 必须使用相同 deterministic workload、Release 配置和 10 repetitions，并以 median 为主要比较值。
- 优化完成后要重新 profile；旧 hotspot 消失后，新的 dominant cost 才有意义。
