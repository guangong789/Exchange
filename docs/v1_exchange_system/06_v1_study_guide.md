# Exchange / Gateway v1 Study Guide

## 一页 mental model

Client 发送 line-based command。I/O thread 用 non-blocking socket 和 level-triggered epoll 读取 bytes，每个 connection 的 `LineFramer` 恢复完整 line，`parse_command()` 生成 owning `CommandParseResult`。

I/O thread 不执行 matching，而是把 `CommandEnvelope{ConnectionId, request}` 放入 bounded command queue。唯一 matching thread 按 FIFO 顺序执行 `MatchingEngine`；`OrderBook` 维护 Price-Time Priority state，`EventCollector` 保存本 command 的 ordered Events。Worker 编码 response，把 `ResponseEnvelope{ConnectionId, string}` 放入 bounded response queue，再写 eventfd 唤醒 I/O thread。

I/O thread 用 stable `ConnectionId` 路由 response。Connection 仍 live 时，把 bytes 追加到 per-connection write buffer；EPOLLOUT ready 后用 `send(MSG_NOSIGNAL)` 处理 partial writes。Connection 已关闭时丢弃 response，但已经执行的 matching state 不回滚。

整个设计的核心不是“用了两个 threads”，而是以下边界都明确：

```text
Socket state belongs to I/O thread.
Order-book state belongs to matching thread.
Only owning value objects cross bounded queues.
One matching thread defines total execution order.
Backpressure is bounded and has explicit failure behavior.
```

## Self-check questions

这些问题应当用完整因果链回答，而不是只说“因为更快”或“因为 thread-safe”。

### Matching

1. 为什么 `OrderBook` 同时使用 ordered price levels、per-level FIFO lists 和 `order_index_`？每种结构分别解决什么问题？
2. BUY 和 SELL 的 crossing condition 分别是什么？为什么成交价使用 maker price？
3. 当前 time priority 到底由什么决定？`Timestamp` 为什么不参与同价 FIFO 排序？
4. 一条 incoming order 如何产生 multi-level、partial/full fills？Maker partial fill 和 taker remainder 分别如何留在 book 中？
5. `OrderLocation` 保存 list iterator 带来什么 cancel complexity？又依赖什么 iterator-stability 和 memory-allocation tradeoff？

### Events / replay

6. 为什么 `OrderBook` 是 state truth owner，而 Event generation 放在 `MatchingEngine`？
7. 一次 multi-fill Add 的精确 Event 顺序是什么？`filled_quantity` 是 per-trade 还是 cumulative，为什么？
8. `AddOrderExecutionResult::last_trade_maker_remaining_quantity` 为什么足够避免每笔 Trade 后重新 lookup maker？
9. ReplayEngine 所说的 deterministic 到底保证什么？Invalid Add 和 failed Cancel 分别会怎样影响后续 replay？

### Networking / protocol

10. 为什么 TCP `recv()` 的一次返回不能代表一条 command？`LineFramer` 如何处理 split command、multiple commands 和 incomplete tail？
11. Line protocol 对 spaces、case、LF/CRLF、integer parsing 和 max line length 有哪些精确规则？
12. 为什么 non-blocking recv/send 必须处理 `EINTR`、`EAGAIN/EWOULDBLOCK` 和 partial progress？
13. 为什么 LT epoll 下仍应循环 accept/recv/send 到 `EAGAIN`？为什么 v1 暂不需要 EPOLLET？
14. 为什么 EPOLLOUT 不能永久启用？`write_buffer` 和 `write_offset` 如何保留 response byte order？
15. 为什么使用 `MSG_NOSIGNAL`？`EPOLLRDHUP`、`EPOLLHUP` 和 peer EOF 的语义有什么不同？

### Concurrency

16. 为什么引入 matching thread 后，MatchingEngine 和 OrderBook 仍然不需要内部 locks？
17. I/O thread 与 matching thread 分别拥有哪些 mutable state？为什么 ownership boundary 比“到处加 mutex”更重要？
18. `CommandEnvelope` / `ResponseEnvelope` 为什么不能包含 raw fd、pointer、reference 或指向 connection buffer 的 `string_view`？
19. 为什么 malformed input 也必须进入同一个 command queue？如果在 I/O thread 立即回复会破坏什么 ordering？
20. `BoundedQueue` 的 condition-variable predicate 如何处理 spurious wakeup、queue close 和 FIFO semantics？

### Backpressure / lifecycle

21. 为什么 command queue producer 使用 `try_push()`，而 response queue producer使用 `wait_push()`？两种选择分别保护谁不被阻塞？
22. Command queue、response queue 和 per-client 64 KiB output limit 分别限制什么资源？Full/overflow 时各自发生什么？
23. Client half-close 后，为什么 connection 只有在 `read_closed && in_flight_requests==0 && no pending output` 时才能关闭？
24. 为什么 raw fd 不适合作为 asynchronous identity？`ConnectionId` 和两张 map 如何阻止 fd reuse misrouting？
25. `request_stop()` 如何同时唤醒 blocked consumer、blocked producer 和 `epoll_wait()`？为什么 worker exception 必须用 `exception_ptr` 回到 calling thread？

### Performance / benchmarking

26. Core、in-memory E2E 和 real-TCP gateway benchmark 各包含/排除了什么？为什么不能把三者吞吐直接当成同一指标？
27. Throughput window=8 和 closed-loop latency window=1 分别制造什么 load model？为什么 latency 从 final request byte sent 开始？
28. 为什么 benchmark 使用 warmup、fresh gateway per repetition、disjoint OrderId ranges、median 和 CV？
29. 为什么 Experiment 3 的 neutral/noisy result 仍然有价值？“largest hotspot”为什么不自动等于“必须优化”？

### System design

30. 给定多个同时 ready clients，哪些 ordering 是 deterministic 的，哪些不是？Command queue 在哪里建立 global total execution order？
31. 为什么更多 clients 能提高 aggregate throughput，却同时恶化 p95/p99，而不能让 single matching thread 并行执行 commands？
32. `HEALTHY TO FREEZE` 表示什么？为什么当前 profiling evidence 不支持 lock-free queue、allocator redesign、`io_uring`、batching、CPU affinity 或 container redesign？

## If you can explain these, you understand v1

逐项确认能够画图、举例并说出 failure behavior：

- [ ] Price priority：best bid / best ask 的 map ordering
- [ ] Time priority：per-price FIFO insertion order，不是 timestamp sorting
- [ ] Maker/taker、maker-price execution 与 multi-level partial fill
- [ ] `std::map + std::list + order_index_` 的职责和 complexity tradeoff
- [ ] OrderBook state truth 与 MatchingEngine Event generation 的边界
- [ ] 每笔 Trade 对应的 deterministic Event ordering
- [ ] Command replay、fixed seed workload 与 deterministic 的准确含义
- [ ] TCP byte stream、LineFramer、partial read/write 与 response framing
- [ ] LT epoll、dynamic EPOLLOUT 和 eventfd wakeup
- [ ] I/O-thread ownership 与 matching-thread ownership
- [ ] BoundedQueue 的 FIFO、blocking/non-blocking APIs 和 close semantics
- [ ] Stable ConnectionId、fd reuse 与 stale-response discard
- [ ] 三层 backpressure、half-close 和 no-rollback behavior
- [ ] Worker failure propagation、request_stop 和 deadlock-free join
- [ ] Core/E2E/gateway benchmark 区别、throughput/latency tradeoff 与 WSL2 限制

如果这些内容都能脱离代码讲清楚，就不仅知道“项目用了哪些类”，也真正理解了 v1 为什么这样分层、哪些 correctness properties 被测试，以及哪些 optimization 被 evidence 明确推迟。
