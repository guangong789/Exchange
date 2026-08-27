# 当前架构 Walkthrough

这份笔记用于理解仓库当前已经实现的行为。所有描述都以当前 production code 和 tests 为准，不把后续设想当成现状。

当前系统的核心形态是：一个线程负责 Linux `epoll` 网络 I/O，一个专用 matching thread 串行执行所有 `Command`。两个线程之间通过有界 `BoundedQueue` 传递 value object，并用 `eventfd` 通知 I/O 线程处理新响应。

## 1. 先看代码地图

### 1.1 Core matching

| 文件 | 职责 |
|---|---|
| `include/exchange/types.hpp` | 定义整数类型 `OrderId`、`Price`、`Quantity`、`Timestamp`，以及 `Side`、`OrderType`。价格和数量不使用 floating point。 |
| `include/exchange/order.hpp` | 定义输入和簿内状态使用的 `Order`。 |
| `include/exchange/trade.hpp` | 定义撮合结果 `Trade`。 |
| `include/exchange/order_book.hpp`、`src/order_book.cpp` | 保存 bids、asks 和 `order_index_`，实现 validation、price-time matching、partial fill、rest 和 cancel。它是订单状态的唯一 owner。 |
| `include/exchange/event.hpp` | 定义五种 event payload 和 `EventPayload` variant。 |
| `include/exchange/event_collector.hpp` | 用 `std::vector<Event>` 按 publish 顺序收集 event。 |
| `include/exchange/matching_engine.hpp`、`src/matching_engine.cpp` | 在 `OrderBook` 外组织 event semantics；撮合本身仍由 `OrderBook` 完成。 |

当前没有独立的 Risk module。网络请求解析成功后会直接进入 `MatchingEngine`。

### 1.2 Replay、workload 和 benchmark

| 文件 | 职责 |
|---|---|
| `include/exchange/command.hpp` | 定义 `AddOrder`、`CancelOrder` 和 `CommandPayload`。 |
| `include/exchange/replay_engine.hpp`、`src/replay_engine.cpp` | 按 `std::vector<Command>` 的顺序同步调用同一个 `MatchingEngine`。 |
| `include/exchange/workload_generator.hpp`、`src/workload_generator.cpp` | 用固定 config 和 seed 生成 deterministic workload；内部用 shadow `MatchingEngine` 跟踪 active order，尽量生成有效 cancel。 |
| `benchmarks/exchange_benchmark.cpp` | 提供 direct `OrderBook` Core benchmark 和 `ReplayEngine -> MatchingEngine -> OrderBook -> EventCollector` End-to-End benchmark。 |

### 1.3 Protocol

| 文件 | 职责 |
|---|---|
| `include/exchange/line_protocol.hpp`、`src/line_protocol.cpp` | `LineFramer` 处理 TCP byte stream 的换行 framing；`parse_command()` 处理 grammar；`encode_success()` / `encode_error()` 生成 line-based response。 |

### 1.4 epoll transport 和 gateway

| 文件 | 职责 |
|---|---|
| `include/exchange/epoll_server.hpp`、`src/epoll_server.cpp` | 管理 listening socket、client socket、`epoll`、`eventfd`、per-connection framing/write state 和连接关闭。只关心 transport，不执行撮合。 |
| `include/exchange/tcp_gateway.hpp`、`src/tcp_gateway.cpp` | 把 protocol、两个 bounded queue、matching thread 和 `EpollServer` 组合起来。 |
| `include/exchange/bounded_queue.hpp` | 基于 `std::deque`、`std::mutex`、`std::condition_variable` 的 generic bounded FIFO queue。 |
| `apps/gateway_main.cpp` | 很薄的 `exchange_server <port>` 入口：校验端口、构造 `TcpGateway`、打印监听地址并调用 `run()`。 |

CMake target 的依赖方向是：

```text
exchange::core
      ^
      |
exchange::protocol
      ^
      |
exchange::gateway ---- Threads::Threads
      ^
      |
exchange_server
```

## 2. Protocol 先把 byte stream 变成 value object

### 2.1 Framing

TCP 只提供有序 byte stream，不保留应用层消息边界。每个 `ConnectionState` 因此持有自己的 `LineFramer`：

- `append(bytes)` 追加一次 `recv()` 得到的 bytes。
- `next_line()` 每次取出一条以 `\n` 结尾的完整消息。
- 一条命令可以跨多个 `recv()`；一次 `recv()` 也可以包含多条命令。
- `\r\n` 会被当作一条正常换行，返回内容不含末尾 `\r`。
- 默认最大 line length 是 256 bytes。

当前 oversized line 的实际网络行为是：`LineFramer` 返回 `LineTooLong`，`EpollServer` 请求关闭该连接。虽然 protocol 层可以编码 `ERR LINE_TOO_LONG`，当前 transport path 并不会先发送这条错误响应。

### 2.2 Command grammar

支持的 grammar 是：

```text
ADD <id> <BUY|SELL> <price> <quantity> <timestamp>
CANCEL <id>
```

规则很严格：命令和 side 使用大写，token 之间只能有一个空格，不能有前后空格或额外 token。整数用 `std::from_chars` 解析。

`parse_command()` 只负责语法和整数解析，不负责 business validation。例如 `ADD 0 BUY 0 -3 -1` 可以解析成 `Command`，随后由 `OrderBook` 拒绝无效的 ID、price 或 quantity。

解析结果是：

```cpp
using CommandParseResult = std::variant<Command, ProtocolError>;
```

Malformed input 也会成为一个 value object 并进入 command queue，而不是在 I/O 线程立即回复。这样 malformed 和 valid command 共享同一条 FIFO execution/response path。

## 3. 一条 ADD 请求的完整生命周期

以下用实际请求说明全链路：

```text
ADD 2 BUY 10100 2 1001\n
```

先假设 shared `OrderBook` 为空，所以该订单不会成交，而是以剩余数量 2 rest 在 bid side。假设这条连接当前的 `ConnectionId` 是 7；真实数值由 accept 顺序决定。

| 步骤 | 所在线程 | 实际入口 | 传递的内容 |
|---:|---|---|---|
| 1 | I/O thread | `EpollServer::poll_once()` | `epoll_wait()` 返回 client fd 的 `EPOLLIN`。 |
| 2 | I/O thread | `EpollServer::read_connection()` | `recv()` 得到 raw bytes：`"ADD 2 BUY 10100 2 1001\n"`。 |
| 3 | I/O thread | `LineFramer::append()` / `next_line()` | 得到拥有自身 storage 的完整 line：`"ADD 2 BUY 10100 2 1001"`。 |
| 4 | I/O thread | `TcpGateway::handle_line()` | `parse_command()` 生成 `Command{AddOrder{Order{2, Buy, Limit, 10100, 2, 1001}}}`。 |
| 5 | I/O thread | `command_queue_.try_push()` | 入队 `CommandEnvelope{connection_id=7, request=<CommandParseResult>}`；envelope 不含 fd、pointer 或 `string_view`。 |
| 6 | I/O thread | `EpollServer::mark_request_in_flight(7)` | 将该连接的 `in_flight_requests` 加一。 |
| 7 | matching thread | `command_queue_.wait_pop()` | 按 FIFO 取得刚才的 `CommandEnvelope`。 |
| 8 | matching thread | `TcpGateway::matching_loop()` | 先 `event_collector.clear()`，建立本次 command 的 event boundary。 |
| 9 | matching thread | `execute_request()` / `execute_command()` | 从 variant 中取出 `AddOrder`，调用 `MatchingEngine::add_order()`。 |
| 10 | matching thread | `OrderBook::add_order_with_execution_result()` | validation 通过；`match_buy()` 找不到 ask；`rest_order()` 把订单放进 `bids_[10100]` 和 `order_index_`。 |
| 11 | matching thread | `MatchingEngine::add_order()` | publish 一个保存原始订单 snapshot 的 `OrderAccepted`。 |
| 12 | matching thread | `encode_success()` | 把本次 collector 中的一个 event 编码为 response string。 |
| 13 | matching thread | `response_queue_.wait_push()` | 入队 `ResponseEnvelope{connection_id=7, encoded_response=<string>}`。 |
| 14 | matching thread | `EpollServer::notify()` | 向 non-blocking `eventfd` 写入 `uint64_t{1}`，唤醒可能阻塞在 `epoll_wait()` 的 I/O thread。 |
| 15 | I/O thread | `drain_wakeup()` / `TcpGateway::handle_wakeup()` | 先把 eventfd drain 到 `EAGAIN`，再用 `response_queue_.try_pop()` drain 当前可用响应。 |
| 16 | I/O thread | `EpollServer::queue_write(7, response)` | 用 `ConnectionId` 找到当前 live connection，把 bytes append 到它的 `write_buffer`。 |
| 17 | I/O thread | `EpollServer::complete_request(7)` | `in_flight_requests` 减一；如果 peer 已 half-close，也会重新检查是否可以关闭。 |
| 18 | I/O thread | `update_interest()` | 因为现在存在 pending output，给该 client socket 打开 `EPOLLOUT` interest。 |
| 19 | I/O thread | `write_connection()` | socket writable 时循环 `send(..., MSG_NOSIGNAL)`；处理 partial write、`EINTR` 和 `EAGAIN`。 |
| 20 | I/O thread | `update_interest()` | response 发完后清空 write state，并关闭不再需要的 `EPOLLOUT` interest。 |

返回给 client 的 bytes 是：

```text
OK 1
EVENT ORDER_ACCEPTED 2 BUY 10100 2 1001
```

这里有一个容易忽略的时序点：当前代码先把 envelope 放入 command queue，再增加 `in_flight_requests`。worker 可能马上 pop，但 response 只能由同一个 I/O thread 在当前 line callback 返回后处理，因此 `complete_request()` 不会抢在 `mark_request_in_flight()` 前执行。

## 4. Thread ownership

| 状态或组件 | Owner / 使用者 | 为什么不会形成 data race |
|---|---|---|
| `EpollServer` 的 listener、epoll fd 和 client socket 操作 | I/O thread | accept、recv、send、interest 修改和 close 都在 event loop 上串行发生。 |
| `ConnectionState` | I/O thread | 不跨线程传 pointer/reference；worker 只看到 `ConnectionId`。 |
| 每个连接的 `LineFramer` | I/O thread | 只在该连接的 `read_connection()` 中访问。 |
| `connections_`（fd -> state）和 `connection_fds_`（ID -> fd） | I/O thread | accept、lookup、deferred cleanup 都由 I/O thread 完成。 |
| `MatchingEngine`、内部 `OrderBook` | matching thread | 它们是 `matching_loop()` 的 stack local，只由唯一 worker 顺序调用。 |
| `EventCollector` | matching thread | 同样是 `matching_loop()` 的 stack local，并按 command clear/reuse。 |
| `command_queue_` | I/O producer，matching consumer | `BoundedQueue` 用 mutex 和 condition variable 保护内部 deque。 |
| `response_queue_` | matching producer，I/O consumer | 同上。 |
| `eventfd` | matching/control path 写，I/O thread 读 | Linux eventfd 提供跨线程 counter/wakeup；`notify()` 不接触 connection state。 |
| `stop_requested_` | control/I/O/matching paths | 使用 `std::atomic_bool` 的 release/acquire。 |
| `worker_failure_` | matching thread 写，I/O/calling thread 读 | worker 先写 `exception_ptr`，再 release-store `worker_failure_ready_`；读取端 acquire-load 后才访问它。 |

最重要的 ownership boundary 是：socket state 不去 matching thread，order-book state 不回 I/O thread。跨线程只传可独立存活的 value object。

## 5. BoundedQueue：两个线程之间的边界

`BoundedQueue<T>` 内部是固定 positive capacity 的 `std::deque<T>`，由一个 mutex 保护，并有两个 condition variable：

- `not_empty_`：有新 item 或 queue close 时唤醒 consumer。
- `not_full_`：pop 释放 capacity 或 queue close 时唤醒 producer。

它保持 `push_back()` / `pop_front()` 的 FIFO ordering。

### 5.1 五个操作的实际语义

| API | 当前语义 |
|---|---|
| `try_push(T value)` | 不等待。queue 未关闭且未满时入队；full 或 closed 都返回 `false`。成功后通知一个 consumer。 |
| `wait_push(T value)` | full 时阻塞；有空间后入队。若成功前 queue 被关闭，返回 `false`。 |
| `try_pop()` | 不等待。立即取最老 item；empty 返回 `std::nullopt`。成功后通知一个 producer。 |
| `wait_pop()` | empty 时阻塞；有 item 时取最老 item。queue 已关闭且没有 item 时返回 `std::nullopt`。 |
| `close_and_discard()` | 幂等地标记 closed、清空所有 queued item、唤醒全部 producer/consumer；以后所有 push 都失败。 |

`close_and_discard()` 不是 graceful drain。它明确选择 discard queued work，用于当前最小 shutdown/fatal-failure lifecycle。

### 5.2 为什么两条 queue 使用不同的 push

Command path 使用 `try_push()`：I/O thread 不能因为 matching 暂时跟不上而阻塞，否则 accept、read、write 和其他 client 都会一起停住。queue full 时只关闭产生 overflow 的 client。

Response path 使用 `wait_push()`：matching thread 可以在 response queue full 时等待 I/O thread drain，以保持已有 command 的 response，不让跨线程 response memory 无界增长。I/O thread pop 后会通知 `not_full_`，worker 继续。

## 6. ConnectionId、fd reuse 与连接生命周期

### 6.1 为什么不能把 raw fd 放进异步 envelope

fd 只是进程 fd table 中当前 slot 的整数。连接关闭后，OS 可以很快把相同数字分配给新连接。如果一个旧请求只携带 fd，延迟返回的 response 可能被写给碰巧复用该 fd 的新 client。

因此 raw fd 只用于 I/O thread 内的 socket/epoll syscall；异步边界使用：

```cpp
using ConnectionId = std::uint64_t;
```

每个 `EpollServer` 用 `next_connection_id_{1}` 单调分配 ID：

- `0` 永远无效。
- ID 在该 server instance 生命周期内不复用。
- 分配到 `uint64_t` 最大值后把 counter 置为 0；后续 accept 会直接关闭新 fd，而不会 wrap/reuse ID。

### 6.2 两张 map

```text
connections_      : raw fd       -> ConnectionState
connection_fds_   : ConnectionId -> current raw fd
```

异步 response 先用 `connection_fds_` 找 fd，再用 `connections_` 找 state，并再次检查 `state.id == requested_id`。只有实际执行 `close_connection()` 时，两张 map 才一起 erase，此时该 `ConnectionId` 才真正失效。

### 6.3 ConnectionState 中每个字段的含义

| 字段 | 含义 |
|---|---|
| `fd` | 当前 socket fd，只供 I/O thread 使用。 |
| `id` | 稳定的异步 routing identity。 |
| `framer` | 该 client 尚未完整 framing 的 input bytes。 |
| `write_buffer` / `write_offset` | 已编码但尚未全部 `send()` 的 bytes，以及下一个待发送位置。 |
| `in_flight_requests` | 已进入 command queue、但 response 尚未在 I/O thread 完成 routing 的请求数。 |
| `read_closed` | peer 已发送 EOF/half-close；不再监听 `EPOLLIN`，但允许等待 response 和 flush output。 |
| `close_requested` | 已决定关闭；实际 erase/close 延迟到安全 event-loop boundary。 |

`close_requested` 不等于 ID 已失效。`queue_write()` 触发 output overflow 时可以把它设为 true，但不能当场 erase state；紧接着的 `complete_request()` 仍必须能找到同一个 state 并减少 in-flight count。

### 6.4 四种典型生命周期

#### 正常连接和正常关闭

1. `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` 创建 client fd。
2. 分配新 `ConnectionId`，注册 `EPOLLIN | EPOLLRDHUP`，写入两张 map。
3. client 正常收发多条 command/response。
4. peer EOF 后设 `read_closed`。
5. 当 `in_flight_requests == 0` 且没有 pending output 时关闭。
6. `close_connection()` 从 epoll 删除 fd、`close(fd)`，再 erase 两张 map。

#### Half-close 时仍有 outstanding request

client 执行 `shutdown(SHUT_WR)` 后，server 仍可能有 command 正在 matching queue/worker 中。关闭条件因此是：

```text
read_closed
&& in_flight_requests == 0
&& no pending socket output
```

只看 EOF 会过早关闭 socket，导致稍后生成的合法 response 无法 flush。

#### response 返回前 client 已断开

fatal disconnect/RST 可以先移除两张 map。只要 command 已经成功进入 command queue，worker 仍会按 FIFO 执行它；I/O thread 随后处理 response 时，`queue_write(old_id, ...)` 找不到 live state，response 被丢弃，`complete_request(old_id)` 返回 `false`。已经发生的 matching state change 不会 rollback。

#### fd 被新 client 复用

新 client 会得到新的 `ConnectionId`。旧 response 带的是旧 ID，无法通过 `connection_fds_` 找到新连接，因此即使 raw fd 数字相同也不会 misroute。

## 7. epoll 与 eventfd 的工作方式

### 7.1 Listening socket

`EpollServer` 构造时依次完成：

1. `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`；
2. `SO_REUSEADDR`；
3. `bind()` 到 `127.0.0.1:<port>`；
4. `listen(SOMAXCONN)`；
5. `getsockname()`，因此测试传 port 0 时能取得真实端口；
6. `epoll_create1(EPOLL_CLOEXEC)`；
7. 把 listener 以 `EPOLLIN` 注册到 epoll；
8. 创建并注册 non-blocking `eventfd`。

Listener ready 后，`accept_connections()` 循环 `accept4()`，直到 `EAGAIN/EWOULDBLOCK`。`EINTR` 重试，`ECONNABORTED` 跳过该次连接后继续 accept。

### 7.2 当前是 level-triggered

代码没有使用 `EPOLLET`，所以当前是 level-triggered epoll。即便如此，accept、recv、send 都尽量循环到 `EAGAIN`，让每次 readiness 被完整消费，也减少不必要的重复 wakeup。

### 7.3 Client event

- `EPOLLIN`：`recv()` 到 `EAGAIN`，交给该连接的 `LineFramer`，逐条调用 line handler。
- `EPOLLOUT`：只在有 pending output 时打开，`send(MSG_NOSIGNAL)` 到发完或 `EAGAIN`。发完立即关闭这项 interest，避免 writable socket 导致 busy loop。
- `EPOLLRDHUP`：先尝试消费仍可读的数据，再进入 graceful half-close 状态。
- `EPOLLERR` / `EPOLLHUP`：属于 fatal socket state。若同时带可读/hangup flags，代码先调用 read path 尽量消费数据，然后关闭；pending output 可以被丢弃。
- fatal `recv()` / `send()` error：请求关闭该 client；不会 rollback 已经执行的 matching command。

Partial write 通过 `write_offset` 保存进度。下一次 `EPOLLOUT` 从未发送位置继续，而不是重发整个 response。

### 7.4 eventfd 是 response 和 shutdown 的 wakeup doorbell

matching thread 完成 response enqueue 后调用 `notify()`。`notify()` 是 `noexcept`：

- 完整写入 8-byte counter 后返回；
- `EINTR` 重试；
- `EAGAIN/EWOULDBLOCK` 直接返回，因为 eventfd 已经处于 readable 状态；
- 其他 write failure 也不抛异常。

多个 notify 可以 coalesce 成一次 readable event。I/O thread 不假设“一次 write 对应一次 epoll event”，而是：

1. `drain_wakeup()` 反复 read eventfd 到 `EAGAIN`；
2. 调用 `WakeHandler`；
3. `handle_wakeup()` 用 `try_pop()` 把当时 response queue 中可用的 item 全部 drain。

`request_stop()` 也使用同一个 eventfd，确保阻塞在 `epoll_wait(-1)` 的 I/O thread 能醒来检查 stop state。

## 8. Matching semantics

### 8.1 Core 调用关系

```text
Command
  -> MatchingEngine
       -> OrderBook
            -> zero or more Trade
       -> EventCollector
            -> ordered Events
```

`OrderBook` 是 matching-state truth 的 owner；`MatchingEngine` 不复制订单簿状态，只使用 `OrderBook` 的 execution result 生成 events。

### 8.2 OrderBook 的数据结构

```cpp
bids_: std::map<Price, std::list<Order>, std::greater<Price>>
asks_: std::map<Price, std::list<Order>, std::less<Price>>
order_index_: std::unordered_map<OrderId, OrderLocation>
```

- bid 从最高 price 开始，ask 从最低 price 开始。
- 同一 price level 内用 `std::list<Order>` 保持插入 FIFO。
- `order_index_` 保存 side、price 和 list iterator，用于 lookup/cancel。

这里的 time priority 指进入 order book 的先后顺序，不是比较 `Order::timestamp`。即使后插入订单的 timestamp 更小，先插入的订单仍先成交。`timestamp` 会保存在 Order 中，而 `Trade::timestamp` 使用 incoming/taker order 的 timestamp。

### 8.3 Crossing、maker/taker 和成交价格

- incoming BUY 在 best ask `<= buy limit price` 时成交。
- incoming SELL 在 best bid `>= sell limit price` 时成交。
- 已在 book 中的 resting order 是 maker；incoming order 是 taker。
- `Trade::price` 使用 maker/resting price，而不是 taker limit price。
- 每笔成交量是 `min(taker remaining, maker remaining)`。
- maker quantity 归零就从 list 和 `order_index_` 移除；空 price level 从 `std::map` 移除。
- taker 还有剩余 quantity 时，以剩余数量 rest 到自己的 price level。

### 8.4 Validation 和 cancel

Add 会拒绝：

- `id == 0`；
- `price <= 0`；
- `quantity <= 0`；
- 非 `Limit` 类型；
- 与当前 active/resting order 重复的 ID。

这些情况抛出 `std::invalid_argument`，gateway 映射为 `ERR INVALID_ORDER`。Invalid Add 不改变 book，也不 publish event。

`cancel_order_with_result()` 用执行撤单的同一次 `order_index_.find()` 返回 cancellation 时的 remaining `Order` snapshot。成功时移除订单并生成一个 `OrderCancelled`；找不到 ID 时返回 failure、状态不变、不生成 event，gateway 返回 `ERR CANCEL_NOT_FOUND <id>`。

### 8.5 五种 Event 和顺序

- `OrderAccepted`：Add validation 和 matching 成功后发布，保存 submitted order 的原始 quantity。
- `OrderCancelled`：保存 cancellation 时的 remaining order state。
- `TradeCreated`：保存 buyer ID、seller ID、maker price、per-trade quantity 和 taker timestamp。
- `OrderFilled`：该笔 trade 后 remaining 为 0。
- `OrderPartiallyFilled`：该笔 trade 后仍有 remaining quantity。

`filled_quantity` 是本笔 trade quantity，不是 cumulative filled quantity。

一次 Add 的 deterministic event order 是：

```text
OrderAccepted(taker)
for each Trade:
    TradeCreated
    maker OrderFilled / OrderPartiallyFilled
    taker OrderFilled / OrderPartiallyFilled
```

### 8.6 一个 multi-fill 例子

假设 asks 中已有：

```text
maker 1: SELL 100 x 2   // 更优价格
maker 2: SELL 101 x 5
```

随后进入：

```text
taker 3: BUY 101 x 4, timestamp 30
```

结果依次是：

```text
Trade{buy=3, sell=1, price=100, quantity=2, timestamp=30}
Trade{buy=3, sell=2, price=101, quantity=2, timestamp=30}
```

event 顺序是：

```text
OrderAccepted(order 3, original quantity 4)
TradeCreated(3 vs 1, 2 @ 100)
OrderFilled(order 1, filled_quantity 2)
OrderPartiallyFilled(order 3, filled_quantity 2, remaining 2)
TradeCreated(3 vs 2, 2 @ 101)
OrderPartiallyFilled(order 2, filled_quantity 2, remaining 3)
OrderFilled(order 3, filled_quantity 2)
```

Earlier makers 在继续匹配下一 maker 前一定已经 fully filled；只有最后一笔 trade 的 maker 可能仍有 remaining quantity。因此 `AddOrderExecutionResult` 只需要额外返回 `last_trade_maker_remaining_quantity`，不必让 `MatchingEngine` 为每笔 trade 再查 `order_index_`。

## 9. 每条 command 的 EventCollector boundary

`EventCollector` 在 matching thread 内复用，但每条 `CommandEnvelope` 都有清晰边界：

1. 执行前 `clear()`；
2. `CommandParseResult` 是 error 时直接编码 protocol error，不执行 core；
3. valid command 执行并编码 collector 中本 command 的 events；
4. 编码后再次 `clear()`；
5. 执行或编码发生 unexpected exception 时，catch path 也先 `clear()` 再重新抛出。

所以 malformed command、invalid Add、failed Cancel 都不会带入上一条 command 的 events。只有 Add 抛出的 `std::invalid_argument` 被当作 `INVALID_ORDER` client error；其他 unexpected exception 会成为 worker failure，而不会被伪装成 protocol error。

## 10. Ordering 到底保证了什么

### 10.1 单个 client

TCP byte order、per-connection `LineFramer`、command queue FIFO、单 matching thread、response queue FIFO 和 write-buffer append order 串在一起，因此同一 client 的完整命令按接收顺序执行，response bytes 也按相同顺序返回。

Malformed command 同样进入 command queue。例如：

```text
valid ADD
BROKEN
valid CANCEL
```

会按顺序得到 success、`MALFORMED_COMMAND`、success，不会因为 parse error 在 I/O thread 被提前回复而乱序。

### 10.2 多个 client

所有 client 共用一个 matching thread 和一个 `OrderBook`。command queue 中形成全局 FIFO 顺序后，matching execution、events 和 response generation 都有一个明确 total order。

但多个 socket 同时 ready 时，Linux `epoll_wait()` 返回 event 的排列、I/O thread 先处理哪个 fd，以及哪些 bytes 已经抵达，不能被应用层当作跨运行固定顺序。因此：

- queue 之前的 multi-client arrival/interleaving 不保证 deterministic；
- queue 之后的 execution 对给定 FIFO sequence 是 deterministic。

每个 response 用 originating `ConnectionId` routing。不同 client 的网络实际收包时间仍可能不同，这不改变 matching execution order。

## 11. 三层 Backpressure

当前系统有三个互相独立的有界边界。

### 11.1 Command queue capacity

默认 capacity 是 1024。I/O thread 使用 `try_push()`：

- 有空间：envelope 入队，再增加该连接的 in-flight count。
- full 或 closed：当前 command 没有入队，也不会执行；标记该 client 关闭。
- `read_connection()` 看到 `close_requested` 后停止处理该连接 buffer 中后续完整 lines。
- 其他 client 不会因此被关闭。

在 overflow 前已经成功入队的 command 不会因为 client 随后关闭而 rollback；它们仍可能被 worker 执行，response 最终因 ID 已失效而丢弃。

### 11.2 Response queue capacity

默认 capacity 也是 1024。matching thread 使用 `wait_push()`：

- queue full 时 worker 阻塞，直到 I/O thread `try_pop()` 释放空间；
- 被阻塞 command 的 matching 已经执行完成，只是 encoded response 尚未成功入队；
- 正常 I/O drain 后 worker 恢复；
- shutdown/fatal failure 关闭 queue 时，`wait_push()` 返回 `false`，worker 退出。

这会暂时停止后续 matching，但不会阻塞 I/O thread。I/O 仍可 accept/read/write，直到 command queue 也被填满并按上一层策略拒绝 client。

### 11.3 Per-client pending socket output

每个 client 最多允许 64 KiB unsent bytes：

```text
write_buffer.size() - write_offset
```

`queue_write()` 加入新 response 后若会超过限制：

- 清空该连接的 pending output；
- 标记该 client deferred close；
- 不影响其他连接；
- 不 rollback 已经执行的 command。

在当前异步架构里，output overflow 被发现时，后续 command 可能已经进入 shared command queue，甚至已经执行。关闭连接会阻止新的 socket reads，但不会撤销这些已入队 command；它们的 response 会在连接失效后被安全丢弃。这一点与早期同步 transport-only 测试中“同一 read buffer 后续 line 立即停止 callback”的场景不同。

## 12. Shutdown 和 worker failure

### 12.1 正常 stop

`TcpGateway::request_stop()` 是 `noexcept`，可用于唤醒阻塞中的两个线程：

```text
request_stop()
  -> stop_requested_ = true
  -> command_queue_.close_and_discard()
  -> response_queue_.close_and_discard()
  -> server_.notify()
```

之后：

- matching thread 若阻塞在 `wait_pop()`，会收到 `nullopt` 并退出；
- 若阻塞在 full response queue 的 `wait_push()`，会收到 `false` 并退出；
- I/O thread 若阻塞在 `epoll_wait(-1)`，会被 eventfd 唤醒；
- `TcpGateway::run()` 看到 stop flag 后离开 loop；
- `TcpGateway` destructor 再次安全地调用 `request_stop()`，然后 `join()` matching thread；
- 随后 `EpollServer` destructor 关闭所有 client fd、listener、eventfd 和 epoll fd。

当前 shutdown policy 是 close-and-discard，不承诺 drain queue 或 flush 所有 response。

### 12.2 Unexpected matching-thread failure

任何未预期异常都不能逃出 `std::thread` function，否则会触发 `std::terminate()`。`matching_loop() noexcept` 因此用最外层 catch-all：

1. `worker_failure_ = std::current_exception()`；
2. release-store `worker_failure_ready_ = true`；
3. `request_stop()` 关闭 queues；
4. `notify()` 唤醒 I/O thread；
5. worker function 正常返回，不让异常穿过 thread boundary。

I/O/calling thread 在 `handle_wakeup()`、`TcpGateway::poll_once()` 的前后，以及 `TcpGateway::run()` loop 和返回前检查 failure flag，并用 `std::rethrow_exception()` 抛出原异常。这样 failure 不会静默消失，也不会被错误编码成 client input error。

`notify()` 的 `noexcept` 语义很关键：fatal catch path 自己不能在唤醒 I/O 时再次抛异常。

## 13. Determinism 的边界

对固定 initial state 和固定 ordered `CommandEnvelope` sequence，当前 matching path 是 deterministic 的：

- command queue FIFO；
- 只有一个 matching thread；
- `OrderBook` 的 price order 和同价 FIFO 明确；
- Trade 使用 maker price；
- Event 顺序固定；
- response queue 和 per-connection write append 顺序固定。

`ReplayEngine` 的 determinism 更强，因为输入本身就是固定 `std::vector<Command>`，没有 network scheduling。相同 command vector 会得到相同 event stream 和 final book state。

网络服务器的 determinism 从“command 进入 shared queue”开始。两个 client 同时 ready 时谁先被 I/O thread enqueue 受 kernel readiness 和 event-loop processing order 影响，因此跨 client 的 arrival order 不承诺在不同运行中相同。单 matching thread 保证的是：一旦 total order 已形成，执行不会再被并发改写。

## 14. Replay、synthetic workload 与性能观察

### 14.1 Replay 和 workload 的作用

`ReplayEngine` 不做 queue、network 或 event isolation，只按 vector 顺序把 `AddOrder` / `CancelOrder` 直接送进同一个 `MatchingEngine`。Invalid Add 会抛出并停止 replay；failed Cancel 返回 false，但 replay 会继续。

`SyntheticWorkloadGenerator` 使用 `std::mt19937_64`、固定 seed、确定性的 rejection sampling、顺序递增的 unique OrderId 和 `command_index + 1` timestamp。它维护 active ID vector 和 ID-to-index map，并运行 shadow engine；每次 Add 只 reconciliation incoming order 和 trades 涉及的 maker IDs，不全量扫描所有历史 ID。Cancel 在可能时选择当前 active order。

### 14.2 Benchmark 如何分层

- Core：直接执行 `OrderBook::add_order()` / `cancel_order()`，尽量排除 Replay 和 Event overhead。
- End-to-End：`ReplayEngine -> MatchingEngine -> OrderBook -> EventCollector`。
- workload 生成和 dry run 位于 timed benchmark 外。
- 每个 iteration 在 `PauseTiming()` 中重建状态，并 reserve event capacity 和 `order_index_` peak capacity。
- dry run 统计 add count、trade count、event count 和 exact `peak_active_order_count`。

### 14.3 已完成实验的思路和结论

初始 Release baseline 大约是 Core 8–9M commands/s、End-to-End 3.5–4.2M commands/s。绝对时间受 WSL2 环境影响，后续实验都使用相同 deterministic workload、paired Before/After、10 repetitions 和 median。

1. **EventCollector reserve**：profile 先发现 `std::vector<Event>` growth/reallocation。benchmark setup 在 timed section 外 reserve 精确 event count 后，1M E2E median 从 229 ms 降到 169 ms，commands/s 提升 35.52%。这验证的是 capacity growth，而没有改变 Event layout 或 production semantics。
2. **移除 post-trade maker lookup**：`OrderBook` 本来就知道最后一笔 trade 后 maker 的 remaining quantity，于是通过 detailed execution result 返回它，避免 `MatchingEngine` 再做 `find_order()`。Trade/Event layout 不变；1M E2E commands/s 提升 15.30%。
3. **合并 cancellation lookup**：`cancel_order_with_result()` 用执行 cancellation 的同一次 lookup 返回 remaining Order，去掉上层预查。Profile 验证重复 lookup 消失，但 1M E2E 结果落在 noise/CV 范围内，结论是 performance neutral。API 因 ownership 和语义更清晰而保留。
4. **`order_index_` pre-sizing**：benchmark dry run 计算 simultaneous active resting orders 的 exact peak，并在 timed section 外 reserve。timed insertion 的 growth-triggered rehash 消失；1M Core median throughput 提升 8.71%，E2E 提升 7.13%。这应解释为 hash-table pre-sizing 的整体影响，而不只是 `_M_rehash` 指令成本。

这些实验体现的是同一条工程原则：先 profile，再用最小 isolated change 验证一个 hypothesis，最后重新 profile。当前证据还没有把 `std::map`、`std::list`、`std::unordered_map` 的整体替换或 allocator redesign 变成已批准结论，因此更深的 OrderBook redesign 仍是 deferred，而不是当前架构的一部分。

## 15. 当前完整架构图

```text
                     Clients
                        |
              TCP byte stream / socket readiness
                        |
                        v
+--------------------------- I/O THREAD -------------------------------------+
|                                                                            |
|  EpollServer::poll_once                                                    |
|    -> epoll_wait (listener / client sockets / eventfd)                     |
|    -> recv                                                                 |
|    -> per-connection LineFramer                                            |
|    -> TcpGateway::handle_line                                              |
|    -> parse_command                                                        |
|    -> CommandEnvelope{ConnectionId, CommandParseResult}                    |
|                       |                                                    |
+-----------------------|----------------------------------------------------+
                        v
              +-------------------------+
              | bounded command_queue_  |
              | I/O: try_push           |
              | worker: wait_pop        |
              +-------------------------+
                        |
+-----------------------| MATCHING THREAD -----------------------------------+
|                       v                                                    |
|  TcpGateway::matching_loop                                                 |
|    -> EventCollector::clear                                                |
|    -> MatchingEngine                                                       |
|         -> OrderBook                                                       |
|              -> price-time matching / cancel                              |
|              -> Trade(s)                                                   |
|         -> EventCollector::publish                                         |
|    -> encode_success / encode_error                                        |
|    -> ResponseEnvelope{ConnectionId, std::string}                          |
|                       |                                                    |
+-----------------------|----------------------------------------------------+
                        v
              +-------------------------+
              | bounded response_queue_ |
              | worker: wait_push       |
              | I/O: try_pop            |
              +-------------------------+
                        | matching thread: notify()
                        v
              +-------------------------+
              | Linux eventfd counter   |
              | registered as EPOLLIN   |
              +-------------------------+
                        |
                 wakes epoll_wait
                        |
+-----------------------| I/O THREAD ----------------------------------------+
|                       v                                                    |
|  drain_wakeup -> TcpGateway::handle_wakeup                                 |
|    -> resolve stable ConnectionId                                          |
|    -> per-connection write_buffer / write_offset                           |
|    -> enable EPOLLOUT only while output is pending                         |
|    -> send(MSG_NOSIGNAL), preserving partial-write progress                |
|    -> disable EPOLLOUT when empty                                          |
|                       |                                                    |
+-----------------------|----------------------------------------------------+
                        v
                     Clients
```

## 16. 继续开发前的自检问题

以下问题应当能在不看代码的情况下解释清楚：

1. 为什么 raw fd 不能跨过 asynchronous queue boundary？
2. `ConnectionId` 是如何分配、何时失效、又如何防止 fd reuse misrouting 的？
3. 为什么 `close_requested = true` 后不能立刻 erase `ConnectionState`？
4. 一条 TCP command 被拆成多次 `recv()`，或多条 command 合并在一次 `recv()` 时，`LineFramer` 分别如何处理？
5. 为什么 malformed command 也要作为 `CommandParseResult` 进入 command queue？
6. 为什么 command queue 使用 `try_push()`，full 时具体会发生什么？
7. response queue 为什么使用 `wait_push()`，full 时哪个线程阻塞，哪些工作已经发生？
8. 64 KiB pending-output limit 统计的是什么；超限后哪些内容会丢弃、哪些 matching state 不会 rollback？
9. 为什么 half-close 的关闭条件必须同时检查 `read_closed`、`in_flight_requests` 和 pending output？
10. `EPOLLOUT` 为什么只在有 pending bytes 时启用，partial write 如何续传，eventfd 又如何唤醒 I/O thread？
11. `MatchingEngine`、`OrderBook` 和 `EventCollector` 分别由哪个线程拥有，跨线程实际传递哪些类型？
12. Price-time priority 在当前容器中如何实现；`Order::timestamp` 是否参与同价订单排序？
13. 一笔 Trade 的成交价格、timestamp 和 `filled_quantity` 分别来自哪里；multi-fill event 的固定顺序是什么？
14. 当前系统在哪个边界之后保证 deterministic total execution order，为什么同时 ready 的不同 client 在这个边界之前仍可能出现不同顺序？
15. 正常 stop 和 unexpected worker failure 分别如何唤醒两个线程、结束 worker、join，并把原异常带回 I/O/calling thread？
