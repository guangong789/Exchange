# Exchange / Gateway v1：系统总览

## 1. 先建立完整 mental model

这是一个用 C++20 实现的最小加密货币交易所模拟器。v1 的重点不是覆盖完整交易业务，而是把一条订单从 TCP bytes 到 deterministic matching，再到 TCP response 的完整链路做正确，并且让这条链路可以测试、回放、benchmark 和 profile。

系统可以从三个层次理解：

1. **Matching core**：`OrderBook` 保存订单状态并按 Price-Time Priority 撮合；`MatchingEngine` 在状态变化之上生成 ordered Events。
2. **Deterministic tooling**：`Command`、`ReplayEngine` 和 `SyntheticWorkloadGenerator` 提供可重复的输入、事件流和 benchmark workload。
3. **Gateway**：line protocol、non-blocking TCP、level-triggered epoll、bounded queues、单 matching thread 和 eventfd 共同把多个 client 接入同一个 matching core。

最关键的架构约束是：

- socket 和 connection state 只属于 I/O thread；
- `MatchingEngine`、`OrderBook` 和 `EventCollector` 只属于 matching thread；
- 两个线程之间只传递拥有自身生命周期的 value objects；
- 所有订单最终仍由一个 matching thread 串行执行，因此给定同一 command sequence，matching result 是 deterministic 的。

## 2. 各子系统负责什么

### Matching core

`OrderBook` 是订单状态的唯一 truth owner。它负责 order validation、resting order、crossing、partial/full fill、price-level 清理和 cancel。

`MatchingEngine` 包装 `OrderBook`。它不复制 order-book state，而是把撮合产生的 `Trade` 和 execution state 转换成 `OrderAccepted`、`TradeCreated`、fill-state 和 cancellation Events。

### Event system

`Event` 是状态变化对 consumer 的稳定表达。`EventCollector` 是最简单的 in-memory collector，用 `std::vector<Event>` 按 publish 顺序保存事件。OrderBook 只负责状态，Event generation 留在 MatchingEngine。

### Replay

`Command` 用 `std::variant` 表示 `AddOrder` 或 `CancelOrder`。`ReplayEngine` 顺序访问一组 `std::vector<Command>`，调用同一个 `MatchingEngine`，从而复现 event stream 和最终 OrderBook state。

### Deterministic workload generation

`SyntheticWorkloadGenerator` 根据固定 config 和 seed 生成同一 command sequence。它用一个 shadow `MatchingEngine` 跟踪 active order，尽可能让 cancel 指向真实 active ID，避免 baseline workload 被 invalid command 干扰。

### Protocol

`LineFramer` 把任意 TCP byte chunks 恢复成完整 line；`parse_command()` 把 line 解析为 `CommandParseResult`；`encode_success()` 和 `encode_error()` 把执行结果变成 line-based response。

### TCP / epoll transport

`EpollServer` 只关心 transport 和 framing：listener、client socket、epoll readiness、partial read/write、per-connection buffers、`ConnectionId`、half-close 和 cleanup。它不知道 `OrderBook` 或 matching semantics。

### Gateway 与 concurrency boundary

`TcpGateway` 组合 `EpollServer`、protocol 和 matching core：

- I/O thread parse line，并向 bounded command queue 提交 `CommandEnvelope`；
- matching thread 串行执行 command、收集 Events、编码 response；
- bounded response queue 把 `ResponseEnvelope` 送回 I/O thread；
- eventfd 唤醒阻塞在 `epoll_wait()` 的 I/O thread；
- I/O thread 用稳定 `ConnectionId` 找到仍然存活的 connection，再把 response 加入 socket output buffer。

### Benchmarking 与 profiling

项目有两类 benchmark：

- `exchange_benchmark`：Google Benchmark，测 Core 和 in-memory End-to-End replay；
- `exchange_gateway_benchmark`：standalone real-TCP harness，测真实 gateway path 的 throughput、closed-loop latency 和 backpressure behavior。

Linux `perf` 用于观察 function/call-graph hotspot。当前 WSL2 环境适合做 relative comparison，不适合宣称 bare-metal exchange latency。

## 3. Repository / module map

| Layer | 重要文件 | Ownership / 职责 |
|---|---|---|
| Core model | `types.hpp`, `order.hpp`, `trade.hpp` | 定义整数型 ID、price、quantity、timestamp，以及 `Order`、`Trade`、`Side`、`OrderType` value types。 |
| Matching state | `order_book.hpp/.cpp` | 拥有 bid/ask price levels、FIFO order queues 和 `order_index_`；执行 add、match、rest、cancel。 |
| Events | `event.hpp`, `event_collector.hpp` | 定义 event payload variant；按 publish 顺序收集 Events。 |
| Matching facade | `matching_engine.hpp/.cpp` | 调用 OrderBook，并把 execution result 转成 deterministic event sequence。 |
| Commands / replay | `command.hpp`, `replay_engine.hpp/.cpp` | 定义 command variant，并顺序 replay 到 MatchingEngine。 |
| Workload | `workload_generator.hpp/.cpp` | 用固定 seed/config 生成可复现、尽量有效的 Add/Cancel workload。 |
| Protocol | `line_protocol.hpp/.cpp` | line framing、command grammar/parser、success/error/Event response encoding。 |
| Concurrency primitive | `bounded_queue.hpp` | mutex + condition_variable 的 fixed-capacity FIFO queue，以及 close/discard semantics。 |
| Linux transport | `epoll_server.hpp/.cpp` | non-blocking listener/client sockets、LT epoll、eventfd、connection state、read/write lifecycle。 |
| Gateway composition | `tcp_gateway.hpp/.cpp` | command/response envelope、两个 bounded queues、matching worker、failure propagation。 |
| Executable | `apps/gateway_main.cpp` | 解析 port、构造 `TcpGateway`、打印 listener address、调用 `run()`。 |
| Core benchmark | `benchmarks/exchange_benchmark.cpp` | deterministic 10K/100K/1M Core 与 replay baseline。 |
| Gateway benchmark | `benchmarks/gateway_benchmark.cpp` | localhost real-TCP throughput、latency、command-pressure、slow-reader scenarios。 |
| Executable specifications | `tests/*.cpp` | 99 个 test cases，覆盖 matching、events、replay、framing、networking、queues、shutdown 和 routing。 |

对应的 CMake targets 是：

```text
exchange::core
exchange::protocol -> exchange::core
exchange::gateway  -> exchange::protocol + Threads
exchange_server    -> exchange::gateway
exchange_benchmark -> exchange::core + Google Benchmark
exchange_gateway_benchmark -> exchange::gateway
exchange_test      -> production libraries + GoogleTest
```

## 4. 完整架构图

```text
                                      Cross-thread value objects
                                  CommandEnvelope / ResponseEnvelope
                                               │
┌──────────────────────────── I/O thread owns ─┼──────────────────────────────┐
│                                              │                              │
│ Clients                                      │                              │
│   │ TCP bytes                                │                              │
│   ▼                                          │                              │
│ non-blocking sockets                         │                              │
│   │ EPOLLIN / EPOLLRDHUP                     │                              │
│   ▼                                          │                              │
│ EpollServer                                  │                              │
│   ├─ ConnectionState                         │                              │
│   │   ├─ fd + stable ConnectionId            │                              │
│   │   ├─ LineFramer                          │                              │
│   │   ├─ write_buffer + write_offset         │                              │
│   │   └─ lifecycle / in_flight_requests      │                              │
│   └─ complete line                           │                              │
│         │                                    │                              │
│         ▼                                    │                              │
│   parse_command()                            │                              │
│         │ CommandParseResult                 │                              │
│         ▼                                    │                              │
└─────────┼────────────────────────────────────┼──────────────────────────────┘
          │ try_push(CommandEnvelope)          │
          ▼                                    │
   bounded command queue                       │
          │ wait_pop()                         │
┌─────────┼──────────────── matching thread owns ─────────────────────────────┐
│         ▼                                                                   │
│   TcpGateway::matching_loop()                                               │
│         │                                                                   │
│         ├─ MatchingEngine                                                   │
│         │     └─ OrderBook                                                  │
│         │                                                                   │
│         ├─ EventCollector                                                   │
│         └─ response encoding                                                │
│                    │ encoded std::string                                    │
└────────────────────┼────────────────────────────────────────────────────────┘
                     │ wait_push(ResponseEnvelope)
                     ▼
              bounded response queue
                     │
                     ├─ eventfd notify ───────────────┐
                     │                                │ wakes epoll_wait
┌────────────────────┼────────────── I/O thread owns ─▼───────────────────────┐
│                    ▼                                                        │
│          handle_wakeup() / try_pop()                                        │
│                    │ resolve stable ConnectionId                            │
│                    ▼                                                        │
│          per-connection write_buffer                                        │
│                    │ EPOLLOUT                                               │
│                    ▼                                                        │
│          send(MSG_NOSIGNAL)                                                 │
│                    │                                                        │
│                    ▼                                                        │
│                 Clients                                                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 5. 一条 ADD 请求如何走完整条链路

假设 client 已连接，并发送：

```text
ADD 2 BUY 10100 2 1001\n
```

| Step | Thread | Class / function | Input | Output / state change |
|---:|---|---|---|---|
| 1 | Client | `send()` | protocol bytes | bytes 进入 TCP stream；一次 send 不保证对应一次 recv。 |
| 2 | I/O | `epoll_wait()` | kernel readiness | client fd 出现 `EPOLLIN`。 |
| 3 | I/O | `EpollServer::read_connection()` | fd | `recv()` 循环到 `EAGAIN`，bytes append 到当前 connection 的 `LineFramer`。 |
| 4 | I/O | `LineFramer::next_line()` | buffered bytes | `LineFrameResult{LineReady, "ADD 2 BUY 10100 2 1001"}`。 |
| 5 | I/O | `TcpGateway::handle_line()` | `ConnectionId`, `string_view` | `parse_command()` 返回持有 `Command{AddOrder{Order...}}` 的 `CommandParseResult`。 |
| 6 | I/O | `command_queue_.try_push()` | `CommandEnvelope{connection_id, request}` | 成功入队后，`mark_request_in_flight()` 增加该连接 outstanding count。 |
| 7 | Matching | `command_queue_.wait_pop()` | queue front | 唯一 matching thread 取得最老 envelope。 |
| 8 | Matching | `MatchingEngine::add_order()` | copied `Order` | `OrderBook` validation、crossing、fills、resting remainder；返回 `std::vector<Trade>`。 |
| 9 | Matching | `EventCollector` | execution result | 发布 `OrderAccepted`，并按每笔 trade 发布 `TradeCreated`、maker fill state、taker fill state。 |
| 10 | Matching | `encode_success()` | 当前 command 的 `span<const Event>` | 得到一个完整 response `std::string`。 |
| 11 | Matching | `response_queue_.wait_push()` | `ResponseEnvelope{connection_id, string}` | response 进入 bounded FIFO；worker 调用 `server_.notify()`。 |
| 12 | I/O | `drain_wakeup()` / `handle_wakeup()` | readable eventfd | drain eventfd，再 drain response queue。 |
| 13 | I/O | `EpollServer::queue_write()` | stable `ConnectionId`, response | 若 connection 仍存活，append 到 `write_buffer`；随后 `complete_request()`。 |
| 14 | I/O | `update_interest()` | connection state | 有 pending output 时动态启用 `EPOLLOUT`。 |
| 15 | I/O | `write_connection()` | pending bytes | `send(MSG_NOSIGNAL)` 循环到发完或 `EAGAIN`；partial write 由 `write_offset` 记录。 |
| 16 | Client | response parser | TCP response bytes | 先读 `OK <event_count>`，再收齐对应数量的 `EVENT` lines。 |

如果没有成交，response 是：

```text
OK 1
EVENT ORDER_ACCEPTED 2 BUY 10100 2 1001
```

如果已经存在可以成交的 SELL，response 会包含 `TRADE_CREATED` 和双方 fill-state Events。执行结果已经进入 OrderBook 后，即使 client 随后断开，也不会 rollback；只会丢弃无法再路由的 response。

## 6. v1 的明确边界

### v1 已包含

- integer `Price` / `Quantity` 的 limit order matching；
- Price-Time Priority、maker-price execution、partial fill、multi-level fill 和 cancel；
- deterministic Event stream；
- in-memory Command replay 和 deterministic synthetic workload；
- line-based TCP protocol；
- Linux non-blocking sockets、level-triggered epoll 和 multiple clients；
- per-connection partial read/write state、half-close 和 64 KiB pending-output limit；
- one I/O thread + one matching thread；
- bounded command/response queues 和明确 backpressure；
- stable `ConnectionId` 与 fd-reuse-safe response routing；
- eventfd wakeup、clean stop 和 worker failure propagation；
- GoogleTest、Google Benchmark、real-TCP benchmark 和 Linux perf workflow。

### v1 有意不包含

- accounts、balances、positions、risk checks、ledger；
- clearing、settlement、fees 或 Web3/on-chain settlement；
- authentication、authorization、TLS；
- persistence、database、recovery log 或 replication；
- market-data broadcast、subscription 或 order-book snapshot protocol；
- REST、WebSocket、JSON 或 binary protocol；
- agent/MCP layer；
- lock-free queue、custom allocator、memory pool；
- `io_uring`、CPU affinity、batching；
- multiple matching threads、sharding 或 thread pool；
- CUDA online matching；CUDA 只适合未来 offline/batched analytics。

因此，v1 是一个完整但刻意受限的 matching/gateway learning system，不是完整交易所业务系统，也不应被描述为 production exchange。
