# Replay、Workload、Benchmark 与 Profiling

## 1. Command model

Replay 和 Gateway 共用同一个 command model：

```cpp
struct AddOrder { Order order; };
struct CancelOrder { OrderId order_id{}; };

using CommandPayload = std::variant<AddOrder, CancelOrder>;
struct Command { CommandPayload payload; };
```

它只表达业务意图，不包含 network fd、buffer、callback 或 EventCollector。Protocol parser 生成 `Command`，ReplayEngine 消费 `Command`，因此 network path 和 in-memory replay 最终调用相同的 MatchingEngine API。

## 2. ReplayEngine 的 deterministic semantics

`ReplayEngine::replay(const std::vector<Command>&)` 从头到尾顺序遍历，用 `std::visit` 分派：

- `AddOrder` -> `MatchingEngine::add_order()`；
- `CancelOrder` -> `MatchingEngine::cancel_order()`。

这里的 deterministic replay 表示：从相同的初始空状态开始，输入完全相同且顺序相同的 commands，会得到相同的：

- matching order；
- Trades；
- Event type、payload 和顺序；
- best bid/ask、active order count 和 remaining quantities。

它不表示多 client 到达 command queue 之前的网络 interleaving 是 deterministic 的。

### Error behavior

- Failed Cancel：MatchingEngine 返回 `false`、不生成 Event，ReplayEngine 忽略返回值并继续下一 command。
- Invalid Add：`std::invalid_argument` 直接传播，replay 在该 command 停止；之前已经执行的状态和 Events 保留，后续 commands 不执行。
- ReplayEngine 不自动 clear EventCollector；collector 保存整段 replay 的 ordered event stream。

## 3. SyntheticWorkloadGenerator

`WorkloadConfig` 控制：

- `command_count`；
- `buy_ratio_bps`；
- `cancel_ratio_bps`；
- `base_price` 和 `price_variation`；
- `min_quantity` / `max_quantity`；
- deterministic `seed`。

Ratio 使用 basis points：`10'000 bps == 100%`。同一 config + seed 会生成完全相同的 command sequence。

### 为什么不用 standard distributions

Generator 使用 `std::mt19937_64`，并自行实现 `sample_below()`：

```text
draw uint64
reject values below rejection_threshold
return value % upper_exclusive
```

这种 rejection sampling 避免简单 modulo 在区间不能整除 `2^64` 时产生 bias，也避免不同 standard-library distribution implementation 影响序列。

### Deterministic IDs 与 timestamps

- Add OrderId 从 1 单调递增，唯一且可复现；
- command index `i` 对应 timestamp `i + 1`；
- price 和 quantity 都从 config 指定的闭区间采样。

### Active order tracking

Generator 内部用 `ActiveOrderIds`：

- `std::vector<OrderId>` 支持随机下标选择；
- `std::unordered_map<OrderId, index>` 支持 average `O(1)` insert/erase；
- erase 用 swap-with-last，避免 vector 中间移动。

同时运行一个 shadow `MatchingEngine`：

1. 如果抽中 Cancel 且 active set 非空，从 active IDs 中均匀选择一个；
2. 执行 shadow cancel 并从 active set 删除；
3. 如果 active set 为空，Cancel 自动回退为 Add；
4. Add 后只 reconcile incoming order 和 returned Trades 涉及的 maker IDs；
5. `find_order()` 判断这些 order 是否仍 active。

它不会在每次 Add 后扫描所有历史 IDs，因此避免了明显的 `O(N^2)` generator path。

Deterministic workload 的价值是：Before/After benchmark 使用完全相同的工作量，差异更可能来自实现变量，而不是输入变化。

## 4. Google Benchmark：Core 与 End-to-End

`benchmarks/exchange_benchmark.cpp` 固定支持：

- 10,000 commands；
- 100,000 commands；
- 1,000,000 commands。

共同 config 为 50% Buy、10% Cancel、base price 100,000、variation 500、quantity 1–100、seed `0x5EED`。

### `BM_OrderBookCore`

Timed section 包含：

- 对每个 `Command` 做 variant dispatch；
- 直接调用 `OrderBook::add_order()` / `cancel_order()`；
- matching、price levels、order index、Trade vector 等 Core 成本。

它尽量排除：

- ReplayEngine；
- MatchingEngine Event generation；
- EventCollector；
- workload generation；
- state construction/destruction；
- `order_index_` reserve setup。

### `BM_EndToEndReplay`

Timed section 是：

```text
ReplayEngine -> MatchingEngine -> OrderBook -> EventCollector
```

它包含 command dispatch、matching、Event construction 和 collector insertion，但不包含 TCP、protocol parsing/encoding 或 gateway concurrency。

每个 iteration 在 `PauseTiming()` 中创建 fresh state：

- `EventCollector` reserve 精确 `event_count`；
- `MatchingEngine` reserve deterministic `peak_active_order_count`；
- 构造 ReplayEngine；
- iteration 结束后在 paused section 销毁 state。

Benchmark dry run 还计算 add count、trade count、event count 和 peak active orders，用 Google Benchmark rate counters 报告 commands/s、adds/s 和 trades/s。

## 5. Performance experiments 1–4

以下结果都来自相同 deterministic 1M workload、Release build、10 repetitions，并以 median 作为主要比较值。WSL2 absolute timing 会波动，所以每个 Experiment 只比较自己的 paired Before/After。

### Experiment 1：EventCollector pre-reserve

**Observation**：E2E profile 中 `std::vector<Event>::emplace_back` 明显，怀疑反复 capacity growth/reallocation 是额外成本。

**Hypothesis**：在 timed replay 前 reserve 精确 event count，应当隔离并移除 vector growth 成本。

**Smallest change**：给 `EventCollector` 增加显式 `reserve(size_t)`；只由 benchmark setup 在 timed section 外调用，不在 production logic 自动猜 capacity。

| 1M E2E | Before | After | Delta |
|---|---:|---:|---:|
| Median elapsed | 229 ms | 169 ms | -26.20% |
| Commands/s | 4.37591M | 5.93003M | +35.52% |

**Conclusion**：vector growth 是 material overhead。After profile 中仍存在 Event insertion，这是存储 Event 的正常成本，不再等同于 reallocation。

### Experiment 2：移除 post-trade maker lookup

**Observation**：Experiment 1 后，`MatchingEngine::add_order()` 为生成 maker fill Event 调用 `OrderBook::find_order(maker_id)`，maker lookup 约占该调用路径 sampled CPU 的 17–18%。

**Hypothesis**：OrderBook 已经知道 final maker remaining quantity，应直接在 execution result 中返回，而不是上层重新 lookup。

**Smallest change**：增加 `AddOrderExecutionResult` 和 `add_order_with_execution_result()`；原 `add_order()` 与 detailed API 共用 `execute_order()`，Trade/Event layouts 不变。

| 1M E2E | Before | After | Delta |
|---|---:|---:|---:|
| Median elapsed | 209 ms | 181 ms | -13.40% |
| Commands/s | 4.78518M | 5.51748M | +15.30% |

当时 41/41 tests passed，After profile 确认 matching path 不再做 post-trade `find_order()`。

**Conclusion**：owner 已知的 execution state 应由 result 返回；这既减少 redundant lookup，也不制造 duplicated state。

### Experiment 3：移除 duplicate cancellation lookup

**Observation**：Cancellation path 曾经是：

```text
MatchingEngine::cancel_order
-> OrderBook::find_order
-> OrderBook::cancel_order
-> order_index_.find
```

**Hypothesis**：用执行 cancel 的同一次 lookup 返回 Order snapshot，可能减少 E2E cancellation overhead。

**Smallest change**：增加 `cancel_order_with_result()`，并让 bool/detailed APIs 共用 `cancel_order_impl()`。

| 1M E2E | Before | After | Delta |
|---|---:|---:|---:|
| Median elapsed | 170.803 ms | 175.026 ms | +2.47% |
| Commands/s | 5.85473M | 5.71347M | -2.41% |

Throughput difference 小于该组 benchmark CV；44/44 tests passed，profile/disassembly 确认 cancellation 只剩一次 index lookup。

**Conclusion**：这是 valid negative/noisy result。不能宣称性能提升，也没有 material regression 证据。Cleaner API 更准确地表达 execution result，因此保留，而不是为追逐 benchmark noise revert。

### Experiment 4：pre-size `order_index_`

**Observation**：Core 和 E2E profile 中 `unordered_map::_M_rehash` 约占 4%。

**Hypothesis**：在 timed execution 前按同时 active order 的精确 peak reserve，可避免 growth-triggered rehash。

**Smallest change**：dry run 记录 `peak_active_order_count`；OrderBook 增加 `reserve_order_capacity()`，MatchingEngine 做 thin forwarding；Core/E2E 在 paused setup 调用。

| 1M path | Before commands/s | After commands/s | Delta |
|---|---:|---:|---:|
| Core | 8.61008M | 9.36044M | +8.71% |
| E2E | 5.85133M | 6.26860M | +7.13% |

Core Before/After CV 较高，positive result 不应过度解读；E2E After CV 约 2%，结果更稳定。After call graph 确认 timed insertion 不再触发 capacity-growth rehash。Paused `reserve()` 自身仍可能在 whole-process profile 出现。

**Conclusion**：结果代表 hash-table pre-sizing 的整体影响，不只是 `_M_rehash` 指令成本。`reserve()` 也改变 bucket allocation timing、bucket count、load factor distribution 和 cache footprint。

## 6. Concurrent gateway benchmark

### 为什么是 standalone executable

`exchange_gateway_benchmark` 启动真实 `TcpGateway`、localhost TCP clients 和一个 benchmark-local non-blocking `poll()` driver。它需要显式 lifecycle、watchdog、protocol validation 和多 client 状态，使用 standalone CLI 比套进 Google Benchmark iteration model 更直接。

Timed path 包含：

```text
client send -> TCP -> epoll -> framing -> parsing
-> command queue -> matching -> Events -> encoding
-> response queue -> eventfd -> EPOLLOUT/send
-> client recv + complete response parsing
```

它也包含同进程 client-driver CPU，因此 whole-process perf 必须按线程/调用图归因。

### `paired-cross-v1` workload

Workload 为固定 price/quantity 的 SELL/BUY pairs：

```text
SELL id=N   price=100000 quantity=10
BUY  id=N+1 price=100000 quantity=10
```

每两条 commands 预期一笔 Trade，最终 book 为空。一个 pair 始终分配给同一 client，pairs 按 `pair_index % client_count` 分配。

没有直接把 SyntheticWorkloadGenerator 的 sequence 分散到 clients，是因为其中 Cancel 依赖特定的先前 active state；cross-client epoll interleaving 会改变全局到达顺序，可能让 cancellation 失效并改变 workload。Paired-cross 能在不依赖固定 cross-client ordering 的情况下验证 response/trade totals。

### Throughput model：window = 8

每个 client 最多有 8 个 outstanding requests。一个 command 只有在 `send()` 接受其最后一个 byte 后才算 outstanding；partial-sent command 不计入 window，也不会开始一个会超过 window 的新 logical command。

Timer 从 measured phase 第一次发送前开始，到最后一个完整 response 被 parser 识别时结束。Workload/string generation、gateway construction、connect 和 warmup 不计入 measured time。

### Latency model：closed-loop window = 1

每个 client 发送一条完整 command，等待完整 response 后才能发下一条。Latency 起点同样是 final request byte 成功传给 `send()`，终点是 `OK/ERR` header 及其所有 Event lines 被完整解析。

使用 `std::chrono::steady_clock`，percentile 使用固定 nearest-rank algorithm。Throughput 与 latency 是不同 load model，不能放在同一数字里直接比较。

### 当前 v1 Release 参考结果

环境：WSL2，本机 localhost，Release，10 repetitions；throughput 每次 1M measured commands，latency 每次 100K，均先 warmup 10K。

| Clients | Window | Median elapsed | Commands/s | Trades/s | CV |
|---:|---:|---:|---:|---:|---:|
| 1 | 8 | 10.709261 s | 93,377.46 | 46,688.73 | 1.06% |
| 4 | 8 | 4.142879 s | 241,379.13 | 120,689.56 | 1.86% |
| 16 | 8 | 2.297388 s | 435,277.01 | 217,638.50 | 1.30% |
| 64 | 8 | 1.632456 s | 612,574.14 | 306,287.07 | 1.85% |

| Clients | Model | Pooled samples | p50 | p95 | p99 | Max |
|---:|---|---:|---:|---:|---:|---:|
| 1 | closed-loop | 1,000,000 | 47.457 us | 85.888 us | 135.161 us | 9,937.399 us |
| 4 | closed-loop | 1,000,000 | 53.122 us | 96.364 us | 152.950 us | 1,681.705 us |
| 16 | closed-loop | 1,000,000 | 99.331 us | 156.102 us | 201.474 us | 812.743 us |
| 64 | closed-loop | 1,000,000 | 303.822 us | 440.248 us | 513.113 us | 1,119.114 us |

**Measured fact**：throughput 一直扩展到 64 clients，同时 p50/p95/p99 随 concurrency 上升，尤其 64 clients 明显增加。

**Interpretation**：更多 clients 和更大的 aggregate outstanding work 能隐藏单连接 round-trip bubbles，提高 I/O/matching utilization；但单 matching thread 仍只能串行执行 commands，排队和 scheduling 会增加 latency，尤其 tail latency。

这不是“多线程版本必须让单 client 更快”的测试。它描述的是 throughput/latency tradeoff。

### Backpressure stress scenarios

`command-pressure` 使用 command/response queue capacity = 1。一个 offender 一次提交 complete-command burst，一个 healthy client 用 sentinel matching 验证：offender 被关闭、gateway 继续运行、healthy response routing 正确、overflow 前 admitted state 没有 rollback。

当前一次 64-command run：全部 validation PASS，约 10.8 ms。这个 elapsed 不是 headline throughput metric。

`slow-reader` 使用正常 1024 queues。Slow client 以 batch=8 受控提交，但不读取 response；server pending output 最终超过 64 KiB，只关闭 slow connection。Healthy client 持续执行 paired orders，最后 cancel slow client 建立的大 SELL sentinel，以 remaining quantity 验证已经执行的 fills 没有 rollback。

当前一次 run 在提交 3,577 条 slow commands 后关闭 slow client，确认 3,568 个 fills 已应用，healthy client 收到 895 个正确 responses；全部 validation PASS。数字受 kernel socket buffering 影响，重要结论是 isolation 和 state preservation，而不是关闭发生在固定 command 数。

## 7. Final perf sanity check

Final profile 对 throughput 的 1/16/64-client、1M-command、single-repetition cases 使用 WSL2 `task-clock` + DWARF call graph。

主要观察：

- matching thread 的 whole-process CPU sample share 大致稳定在 42–44%；
- benchmark client share 从约 22% 增长到约 31%，不能把 whole-process top list 全部归因给 server；
- mutex operations 可见，但 condition_variable/futex contention 没有异常放大；
- eventfd write/read 可见但不 dominant；
- `parse_command` 保持低个位数 self-time；
- response encoding、string formatting、allocation/memory movement 是正常且可见的成本；
- `OrderBook` lookup/tree operations 没有形成 pathological hotspot；
- 没有 unexpected helper 达到约 30–50% self-time，没有 busy loop 或 excessive wakeup 证据。

最终 verdict：

```text
HEALTHY TO FREEZE
```

当前 evidence 不支持为了 v1 引入 lock-free queue、allocator redesign、`io_uring`、batching、CPU affinity 或 container redesign。

## 8. WSL2 performance interpretation

可以合理声称：

- 同一环境中的 relative Before/After change；
- 1/4/16/64 client scaling trend；
- throughput 与 latency load model 的差异；
- call path 和 relative hotspot composition；
- backpressure、routing、queue 和 shutdown behavior。

不可以据此声称：

- bare-metal exchange latency；
- NIC-level performance；
- NUMA behavior；
- hardware cache/branch efficiency；
- production capacity 或 exchange-grade absolute latency。

当前硬件 PMU unavailable，`cycles`、`instructions`、cache-miss、branch-miss 等结论没有数据支持。Absolute max latency 也很容易受 WSL2 host scheduling 干扰。

## 9. What you should be able to explain yourself

1. 为什么 deterministic workload 是 isolated benchmark 的前提？
2. Core 和 End-to-End benchmark 分别包含/排除了什么？
3. 为什么 setup、reserve 和 workload generation 必须放在 timed section 外？
4. 为什么使用 repetitions、median 和 CV，而不是只比较一次运行？
5. Experiment 3 为什么是有价值的 negative result？
6. 为什么优化后必须 fresh profile，不能继续使用旧 hotspot 排名？
7. Throughput window=8 与 latency window=1 测的是哪两种不同系统行为？
8. 为什么 command 的 final byte sent 才能开始 latency 计时？
9. 为什么一个 recv 不能当成一个完整 response？
10. p50、p95、p99 分别帮助观察什么？为什么 tail latency 会随 concurrency 增加？
11. 为什么 paired-cross workload 比随机 Cancel workload 更适合多 client baseline？
12. Benchmark client 和 gateway 在同一 process 会怎样影响 perf 解读？
13. “largest hotspot”为什么不自动等于“应该优化”？
14. 什么叫 saturation？当前数据说明了什么，又没有说明什么？
15. 哪些 WSL2 结论有效，哪些绝对性能主张无效？
