# Concurrency、Backpressure 与 Lifecycle

## 1. 从 synchronous gateway 到当前架构

早期 synchronous path：

```text
epoll I/O thread
  -> parse
  -> MatchingEngine
  -> encode response
  -> socket output
```

只要 matching 或 encoding 在运行，同一个 thread 就不能继续 accept、recv、send 其他 sockets。

当前 v1：

```text
I/O thread
  -> parse
  -> bounded command queue
  -> matching thread
       -> MatchingEngine / OrderBook / EventCollector
       -> encode response
  -> bounded response queue
  -> eventfd
  -> I/O thread
       -> route by ConnectionId
       -> socket output
```

分离的目标是让 I/O thread 在 matching work 执行时继续处理 socket readiness。它不是为了把 OrderBook 变成 concurrent data structure。

MatchingEngine 仍然 single-threaded，因为：

- 所有 commands 在一个 FIFO 上形成 total execution order；
- OrderBook 不需要 locks；
- price-time priority、Events 和 replay semantics 更容易推理；
- 多 matching threads 会立刻引入 partitioning、cross-book ordering 和 synchronization 问题，不属于 v1。

## 2. Thread ownership table

TcpGateway 本身只创建 matching worker。I/O thread 指调用 `TcpGateway::run()` / `poll_once()` 的线程：standalone server 中通常是 main thread；benchmark 中是 `RunningGateway` 创建的 I/O thread。

| State / component | Owner / access | 说明 |
|---|---|---|
| `EpollServer` listener、epoll fd、client socket syscalls | I/O thread | accept、recv、send、interest changes、close 串行发生。 |
| `ConnectionState` | I/O thread | 不跨线程传 pointer/reference。 |
| Per-connection `LineFramer` | I/O thread | 只在该 connection read path 访问。 |
| `parse_command()` | I/O thread | 在 line callback 内把 ephemeral `string_view` 转成 owning value。 |
| `connections_` / `connection_fds_` | I/O thread | fd 和 logical ID 的 lifecycle map。 |
| `command_queue_` | I/O producer；matching consumer | `BoundedQueue` 内部同步。 |
| `MatchingEngine` / `OrderBook` | matching thread | 都是 `matching_loop()` stack local，只由 worker 访问。 |
| `EventCollector` | matching thread | 每个 command clear/reuse。 |
| response encoding | matching thread | Events 在 owner thread 内编码成 owning `std::string`。 |
| `response_queue_` | matching producer；I/O consumer | `BoundedQueue` 内部同步。 |
| eventfd | matching/control 写；I/O 读 | Linux counter/wakeup primitive，不暴露 connection state。 |
| `stop_requested_` | calling/I/O/matching paths | `std::atomic_bool` release/acquire。 |
| `worker_failure_` | matching 写；I/O/calling thread 读 | `worker_failure_ready_` 的 release/acquire 建立 publication order。 |

设计 concurrency 前先定义 ownership，比先添加 mutex 更重要。绝大多数 mutable production state 实际上仍只有一个 thread owner。

## 3. BoundedQueue 的实现与语义

`BoundedQueue<T>` 是 generic fixed-capacity FIFO：

```text
std::deque<T>
std::mutex
std::condition_variable not_empty
std::condition_variable not_full
bool closed
```

Capacity 必须大于 0。所有 queue state 都在 mutex 下访问，成功 push 使用 `push_back()`，成功 pop 使用 `pop_front()`，所以保留 FIFO ordering。

### `try_push(T value)`

- 获取 mutex；
- closed 或 full 时立即返回 `false`；
- 否则 move 到 queue 尾部；
- 解锁后 `not_empty_.notify_one()`。

它永不等待。

### `wait_push(T value)`

用 predicate wait：

```cpp
not_full_.wait(lock, [this] {
    return closed_ || queue_.size() < capacity_;
});
```

Predicate 同时处理正常 wakeup、spurious wakeup 和 close。醒来后如果 closed 返回 `false`；否则插入并 notify consumer。

### `try_pop()`

Empty 时立即返回 `std::nullopt`。有 item 时 move oldest value、pop front，解锁后通知一个 blocked producer。

### `wait_pop()`

Empty 时等待 `closed_ || !queue_.empty()`。有 item 就返回 oldest item；closed 且 empty 时返回 `std::nullopt`，让 consumer loop 退出。

### `close_and_discard()`

- idempotent；
- 在 mutex 下设置 `closed_` 并清空 queued items；
- notify_all blocked producers 和 consumers；
- 之后所有 push 都失败；
- pop 只会看到 empty/nullopt。

这是 **discarding shutdown**，不是 graceful drain。v1 的 stop policy 不承诺执行所有 queued commands 或发送所有 responses。

## 4. CommandEnvelope 与 ResponseEnvelope

```cpp
struct CommandEnvelope {
    ConnectionId connection_id;
    CommandParseResult request;
};

struct ResponseEnvelope {
    ConnectionId connection_id;
    std::string encoded_response;
};
```

它们只包含 owning value objects。绝不跨线程传递：

- raw client fd；
- `ConnectionState*`；
- `EventCollector*` / `MatchingEngine*`；
- reference；
- 指向 `LineFramer` buffer 的 `string_view`。

这样 worker 的执行不依赖 connection object 生命周期。Connection 先关闭也不会让 envelope dangling；response 最后只按 logical `ConnectionId` 尝试 routing。

## 5. Command submission：为什么使用 `try_push`

I/O callback 的顺序是：

```text
parse complete line
-> create CommandEnvelope
-> command_queue.try_push()
-> success: mark_request_in_flight(connection_id)
-> failure: request_close(connection_id)
```

I/O thread 不能在 full business queue 上阻塞。否则一个 producer/client 的 overload 会停止整个 event loop，其他 clients 的 accept/read/write 也无法继续。

Command queue 默认 capacity 1024。Full 时：

- 当前 command 没有 admitted，不执行；
- 只关闭产生 overflow 的 connection；
- current read loop 停止处理该 connection 已 buffered 的后续 lines；
- 其他 clients 继续工作；
- overflow 前已经成功 admitted 的 commands 不 rollback。

Malformed input 也作为 `CommandParseResult` 入同一个 queue。这样同一 client 的：

```text
valid ADD -> malformed line -> valid CANCEL
```

仍按该顺序产生 response，不会因为 I/O thread 立即回复 parse error 而越过前面的 matching work。

## 6. Matching loop：为什么 response 使用 `wait_push`

Worker loop：

```text
wait_pop CommandEnvelope
-> clear EventCollector
-> execute request or encode protocol error
-> encode exactly one response
-> clear EventCollector
-> wait_push ResponseEnvelope
-> eventfd notify
```

Response queue 默认 capacity 1024。使用 `wait_push()` 是为了：

- 不丢失已经执行 command 的 response；
- 不让 cross-thread response memory 无界增长；
- I/O thread pop 后通过 `not_full_` 唤醒 worker。

Tradeoff 是 response queue full 时 matching thread 会停止执行后续 commands。I/O thread 本身仍不阻塞，可以继续 drain sockets/response queue；如果 command queue 随后也满，再按 command-admission policy 关闭 offender。

每个 command 开始和结束都 clear collector。Malformed、invalid Add、failed Cancel 都不会继承上一 command 的 Events。只有 Add 的 `std::invalid_argument` 映射成 `INVALID_ORDER`；unexpected exception 不伪装成 client protocol error。

## 7. Response routing 与 in-flight requests

Matching thread enqueue response 后写 eventfd。I/O thread 醒来后：

1. drain eventfd 到 `EAGAIN`；
2. `response_queue_.try_pop()` 直到当前 empty；
3. `queue_write(response.connection_id, bytes)`；
4. `complete_request(connection_id)`。

如果 ConnectionId 已失效，`queue_write()` no-op，`complete_request()` 返回 `false`。Response 被丢弃，不会落到 fd-reused replacement client。

如果 `queue_write()` 因 64 KiB output limit 设置 `close_requested`，connection state 不会立即 erase，因此随后的 `complete_request()` 仍能 decrement live `in_flight_requests`。Actual close 发生在 deferred cleanup boundary。

## 8. Ordering guarantees

### Per-client ordering

以下环节都是 ordered/FIFO：

```text
TCP byte order
-> per-connection LineFramer order
-> command queue FIFO
-> single matching thread
-> response queue FIFO
-> write_buffer append order
-> TCP byte order
```

所以一个 connection 上完整 commands 的 execution order 和 response byte order与读取顺序一致。Malformed commands 也在同一 ordering chain 中。

### Global matching order

CommandEnvelope 一旦进入 shared command queue，就形成一个 global FIFO。Single matching thread 严格按此顺序修改一个 shared OrderBook，因此 queue 内 sequence 有 deterministic total execution order。

### 不保证什么

多个 sockets 同时 ready 时，Linux `epoll_wait()` 返回顺序、I/O thread 先处理哪个 fd、各 client bytes 到达时间都可能变化。因此：

- queue 之前的 cross-client arrival/interleaving 不保证跨运行相同；
- 不同 clients 实际收到 response 的 wall-clock order 不保证与 global execution order相同；
- 给定已经形成的 queue sequence，matching outcome 才是 deterministic 的。

## 9. 三层 backpressure

| Layer | Bound | Producer behavior | Overflow / full result |
|---|---:|---|---|
| Command queue | 默认 1024 envelopes | I/O `try_push`，不阻塞 | 关闭 offending client；unadmitted command 不执行。 |
| Response queue | 默认 1024 envelopes | Matching `wait_push` | Worker 等待 I/O drain；有界保存 response。 |
| Socket output | 每 connection 64 KiB unsent bytes | I/O append/write | 只关闭 slow client；pending bytes 可丢弃，state 不 rollback。 |

### Command-pressure observed behavior

Dedicated scenario 把 command/response queues 都设为 1。Offender 发送 64 条 complete-command burst，触发 command admission failure。一个 sentinel SELL 在 overflow 前 admitted；healthy client 随后 BUY 并收到与 sentinel 成交的正确 response，再提交另一个 order 验证 gateway 仍响应。

验证结果：offender closed、healthy isolated、admitted state preserved、routing correct、gateway alive，全部 PASS。

### Slow-reader observed behavior

Slow client 先放置一个大 SELL sentinel，再分 batch=8 发送 crossing BUY，但完全不读取 responses。Healthy client 同时持续发送自己的 SELL/BUY pairs 并读取回复。

Slow connection 的 kernel/socket output 最终使 application pending output 超过 64 KiB，只关闭 slow client。Healthy client 最后 cancel sentinel，remaining quantity 确认 slow client 的已执行 fills 仍在 OrderBook 中。

一次当前环境运行在 3,577 slow commands 后关闭 connection，确认 3,568 fills 已应用；healthy client 895 responses 全部正确。触发 command 数受 socket buffering 影响，不是 API guarantee。

## 10. Half-close 与 disconnect

### Half-close

Peer `shutdown(SHUT_WR)` 后 `read_closed=true`，停止 EPOLLIN，但 connection 继续存活，直到：

```text
read_closed
&& in_flight_requests == 0
&& write_buffer has no pending bytes
```

因此最后一批已 admitted commands 仍可完成 matching、routing 和 flush。

### Full disconnect / RST

Fatal socket state 可以立即关闭 connection 并 erase ID mapping。已经 admitted 的 commands 仍在 global matching sequence 中执行，不回滚；response 回到 I/O thread 时旧 ID 已无效，安全丢弃。

“Client 不再能收到 response”与“business state 应回滚”是两件不同的事。v1 选择 matching execution 一旦 admitted 就不因 transport failure rollback。

## 11. Shutdown lifecycle

`TcpGateway::request_stop()` 是 `noexcept`：

```text
stop_requested_ = true
command_queue_.close_and_discard()
response_queue_.close_and_discard()
server_.notify()
```

这同时解除三种可能的等待：

- worker 在 `command_queue.wait_pop()`：queue closed + empty -> `nullopt` -> exit；
- worker 在 full `response_queue.wait_push()`：queue closed -> `false` -> exit；
- I/O/calling thread 在 `epoll_wait(-1)`：eventfd readable -> wake -> loop 检查 stop flag。

`TcpGateway` destructor 会再次安全调用 `request_stop()`，然后 `join()` matching thread。成员 destruction 随后关闭 EpollServer 内所有 client fds、listener、eventfd 和 epoll fd。

当前 stop 是 discard policy，不保证 drain queued commands 或 flush responses。正常 server executable 也没有 signal-handling framework；外部 process termination 不等同于 application-level graceful drain。

## 12. Worker exception handling

Exception 不能逃出 `std::thread` entry，否则会调用 `std::terminate()`。`matching_loop() noexcept` 在最外层 catch all：

1. 把 `std::current_exception()` 保存到 `worker_failure_`；
2. release-store `worker_failure_ready_ = true`；
3. 调用 `request_stop()` 关闭 queues；
4. 用 noexcept eventfd notify 唤醒 I/O thread；
5. thread function 正常返回。

I/O/calling thread 的 `run()`：

- 每轮 poll 前检查 failure；
- WakeHandler drain response 后检查 failure；
- loop 返回前再次检查 failure。

Acquire-load ready flag 后才读取 `exception_ptr`，再 `std::rethrow_exception()`。Unexpected worker failure 因此既不会变成 protocol error，也不会 silently disappear 或触发 `std::terminate()`。

## 13. What you should be able to explain yourself

1. 为什么先定义 thread ownership，再决定哪些地方需要 lock？
2. 为什么 MatchingEngine 移到 worker 后仍保持 single-threaded？
3. BoundedQueue 如何用 predicate wait 正确处理 spurious wakeup？
4. `try_push`、`wait_push` 的使用场景为什么不同？
5. 为什么 command queue full 不能让 I/O thread block？
6. Response queue full 时系统的哪些部分停止、哪些仍继续？
7. `close_and_discard()` 为什么能解除 producer 和 consumer 两类等待？
8. 为什么 envelope 必须只含 owning value objects？
9. Malformed input 为什么也进入 command queue？
10. Stable `ConnectionId` 与 raw fd 的职责边界是什么？
11. Half-close 为什么需要 `in_flight_requests`？
12. Slow client output overflow 后，为什么 matching state 不 rollback？
13. 三层 backpressure 分别保护哪一种有限资源？
14. Per-client ordering、global execution ordering 和 cross-client nondeterminism 有什么区别？
15. eventfd 在 response delivery 和 shutdown 中分别解决什么问题？
16. Worker exception 为什么必须用 `exception_ptr` 回传？
17. 哪些 shutdown waits 可能导致 deadlock，当前 close/notify 顺序如何解除它们？
18. 为什么当前 evidence 不足以支持 lock-free queue？
