# Network Transport 与 Line Protocol

## 1. Protocol grammar

Protocol 是 UTF-8/ASCII-compatible 的 line-based text protocol。每条 request 以 LF (`\n`) 结束；`LineFramer` 也接受 CRLF，并在交给 parser 前去掉末尾 `\r`。

### Requests

```text
ADD <order_id> <BUY|SELL> <price> <quantity> <timestamp>\n
CANCEL <order_id>\n
```

精确规则：

- command 和 side 必须大写；
- token 之间必须恰好是单个 ASCII space；
- 不接受 leading/trailing space、multiple spaces 或 tab；
- token 数量必须完全匹配；
- integer 必须能被 `std::from_chars` 完整解析，不能 overflow，也不能有多余字符；
- `OrderId` 是 unsigned；`Price`、`Quantity`、`Timestamp` 是 signed。

Parser 只负责 grammar 和 integer parsing。比如 `ADD 0 BUY 0 -3 -1` 在语法上可以产生 `Command`；`id/price/quantity` 的业务 validation 由 MatchingEngine/OrderBook 完成。当前 core 不校验 timestamp 正负。

### Successful response

```text
OK <event_count>\n
<exactly event_count EVENT lines>
```

Event lines：

```text
EVENT ORDER_ACCEPTED <id> <BUY|SELL> <price> <quantity> <timestamp>
EVENT ORDER_CANCELLED <id> <BUY|SELL> <price> <remaining_quantity> <timestamp>
EVENT TRADE_CREATED <buy_id> <sell_id> <price> <quantity> <timestamp>
EVENT ORDER_FILLED <id> <BUY|SELL> <per_trade_filled_quantity>
EVENT ORDER_PARTIALLY_FILLED <id> <BUY|SELL> <per_trade_filled_quantity> <remaining_quantity>
```

`OK 0` 是合法编码，不过当前 successful Add/Cancel 通常至少产生一个 Event。

### Error response

```text
ERR MALFORMED_COMMAND
ERR INVALID_ORDER
ERR CANCEL_NOT_FOUND <order_id>
ERR LINE_TOO_LONG
```

当前 transport 有一个需要按源码理解的细节：`encode_error()` 支持 `LINE_TOO_LONG`，但 `EpollServer::read_connection()` 遇到 oversized input line 时直接 request close，没有把该错误写回 socket。因此 v1 network behavior 是 oversized line 关闭该 client，而不是保证收到 `ERR LINE_TOO_LONG`。

## 2. LineFramer：TCP bytes 不等于 command

TCP 是 ordered byte stream，不保留 application message boundary。下面三种情况都正常：

```text
recv #1: "ADD 1 BUY 100"
recv #2: " 5 10\n"
```

```text
recv #1: "CANCEL 1\nCANCEL 2\n"
```

```text
recv #1: "ADD 1 BUY 100 5 10\nCANCEL"
recv #2: " 1\n"
```

每个 `ConnectionState` 拥有独立 `LineFramer`，所以一个 client 的 incomplete tail 不会与另一个 client 混合。

`LineFramer` 内部保存：

- `buffer_`：尚未完全丢弃的 bytes；
- `read_position_`：下一个 line 起点；
- `max_line_length_`：默认 256 bytes；
- sticky `line_too_long_` 状态。

`next_line()` 返回：

- `NeedMoreData`：还没有 LF；
- `LineReady`：返回一条 owning `std::string`；
- `LineTooLong`：超过限制，此后继续返回同一状态。

Max line length 不包含 LF，也不包含 CRLF 中可选的 `\r`。Consumed prefix 在全部消费后清空，或达到 4096 bytes 且超过 buffer 一半时 compact，避免 buffer 无限保留旧 prefix。

## 3. EpollServer 的 socket lifecycle

### Listener setup

构造 `EpollServer` 时：

1. `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`；
2. `setsockopt(SO_REUSEADDR)`；
3. `bind()` 到 `127.0.0.1:<port>`；
4. `listen(SOMAXCONN)`；
5. `getsockname()` 得到实际 bound port，因此 port 0 可用于 tests；
6. `epoll_create1(EPOLL_CLOEXEC)`；
7. listener 以 `EPOLLIN` 注册；
8. 创建 `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`，同样以 `EPOLLIN` 注册。

Listener ready 时，`accept_connections()` 使用：

```cpp
accept4(listen_fd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)
```

它循环到 `EAGAIN/EWOULDBLOCK`；`EINTR` 重试，`ECONNABORTED` 忽略该次连接并继续。

### Level-triggered epoll

v1 没有设置 `EPOLLET`，因此使用 level-triggered (LT) epoll。LT 更容易验证：只要状态仍 ready，epoll 会继续报告。实现仍然让 accept/recv/send 尽量循环到 `EAGAIN`，减少重复 wakeup，也让未来行为更清晰。

### Client interest

新 client 初始注册：

```text
EPOLLIN | EPOLLRDHUP
```

有 pending output 时动态加入 `EPOLLOUT`。Peer read side 已关闭后停止监听 `EPOLLIN`，但如果仍有 response/output，会保留 `EPOLLOUT`。

## 4. EPOLLIN 与 recv loop

`read_connection()` 使用固定 4096-byte stack buffer：

```text
recv > 0       -> append to LineFramer，取出所有 complete lines
recv == 0      -> read_closed = true
EINTR          -> retry
EAGAIN/EWOULDBLOCK -> 本轮结束
其他 error     -> request_close
```

每个 `LineReady` 通过 `LineHandler(ConnectionId, int fd, string_view)` 同步回调到 TcpGateway。`string_view` 只在 callback invocation 内有效；Gateway 必须 parse/copy 成 value object，不能跨线程保存它。

如果 LineHandler 设置 `close_requested`，当前 connection buffer 中后续 complete lines 不再执行。这样 command admission failure 或 output-limit close 不会继续从同一 read loop 提交新 work。

## 5. EPOLLOUT、partial write 与 output buffer

`ConnectionState` 保存：

```cpp
std::string write_buffer;
std::size_t write_offset{};
```

Unsent bytes 是：

```text
write_buffer.size() - write_offset
```

`queue_write()` 按 response 到达顺序 append bytes。如果前面已有 fully/partially consumed prefix，会先 erase prefix 并把 offset 归零，再 append 新 response。

`write_connection()` 调用：

```cpp
send(fd, pending_data, pending_size, MSG_NOSIGNAL)
```

处理规则：

- `send > 0`：增加 `write_offset`，继续尝试；
- `EINTR`：retry；
- `EAGAIN/EWOULDBLOCK`：保留 buffer/offset，等下一次 EPOLLOUT；
- fatal error：request close；
- 全部发送完成：清空 buffer，offset 归零。

`MSG_NOSIGNAL` 防止向已关闭 peer 写入时产生 `SIGPIPE` 终止整个 process。

### 为什么 EPOLLOUT 不能常开

大多数 TCP sockets 在大多数时间都 writable。如果一直监听 EPOLLOUT，LT epoll 会不断返回这些 fd，即使 application 没有 bytes 要写，形成 busy loop。

因此：

- pending output 从空变非空时，`EPOLL_CTL_MOD` 启用 EPOLLOUT；
- output 全部发送后，立即关闭 EPOLLOUT interest。

### 64 KiB pending-output limit

`kMaxPendingOutputBytes == 64 * 1024`，只限制 unsent bytes。如果 append 新 response 会超过限制：

- 清空该 connection 的 pending output；
- 设置 `close_requested`；
- 在安全 event-loop boundary 关闭该 client；
- 不影响其他 clients；
- 不 rollback 已执行的 matching commands。

## 6. ConnectionState 与 lifecycle

```cpp
struct ConnectionState {
    int fd;
    ConnectionId id;
    LineFramer framer;
    std::string write_buffer;
    std::size_t write_offset;
    std::size_t in_flight_requests;
    bool read_closed;
    bool close_requested;
};
```

### 正常连接

accept -> 注册 epoll -> 分配 ID -> read/parse/execute/write。正常 peer EOF 后进入 `read_closed`，等所有异步请求与 output 完成再 close。

### Half-close

Client 可以 `shutdown(SHUT_WR)`，表示不再发送 request，但仍愿意接收 response。Server 的 graceful-close 条件是：

```text
read_closed
&& in_flight_requests == 0
&& no pending socket output
```

因此收到 EOF 不能立即 close。Command 可能已经在 command queue 或 matching thread 中；必须等待 response routing 完成并 flush bytes。

### `close_requested`

这是“已决定关闭”的状态，不代表 ConnectionId 立刻失效。`request_close(ConnectionState&)` 把 ID 放入 `deferred_closes_`，实际 `epoll_ctl(DEL)`、`close(fd)` 和 map erase 在 callback 安全边界执行。

这个设计保证如下调用仍安全：

```text
queue_write(id, response)  // 可能因 output limit 设置 close_requested
complete_request(id)       // 仍能找到 live state 并 decrement
cleanup_deferred_connections()
```

### Fatal flags

如果 EPOLLIN/EPOLLRDHUP 与 hangup 同时出现，代码先尝试消费 readable data，再处理 close。`EPOLLERR`、`EPOLLHUP`、fatal send/recv 可以丢弃 pending output，但已经执行的 matching state 不回滚。

## 7. Stable ConnectionId 与 fd reuse

Raw fd 只是 process fd table 中的 slot number。Connection A 关闭后，OS 可以立刻把同一个 integer 分配给 Connection B。如果异步 response 只保存 fd，A 的延迟 response 可能被错误发送给 B。

v1 使用：

```cpp
using ConnectionId = std::uint64_t;
ConnectionId next_connection_id_{1};
```

它是每个 `EpollServer` instance 内单调递增的 logical identity：

- 0 永远无效；
- server instance 生命周期内不复用；
- 达到 `uint64_t` 最大值后将 counter 置 0，后续 accept 直接拒绝，不 wrap。

两张 map 分工：

```text
connections_    : fd           -> ConnectionState
connection_fds_ : ConnectionId -> fd
```

I/O syscall 用 fd；异步 `CommandEnvelope`/`ResponseEnvelope` 只携带 ConnectionId。Response 返回时先 ID -> fd，再检查 fd -> state 且 `state.id` 一致。关闭 connection 时同时 erase 两张 map，旧 ID 永远不会指向复用 fd 的新 connection。

如果 client 已完全断开，旧 response 查不到 ID，安全丢弃。Matching state 不回滚，也不会 misroute。

## 8. eventfd wakeup

Matching thread 生成 response 时，I/O thread 可能阻塞在 `epoll_wait(-1)`。普通 condition_variable 不能直接出现在 epoll readiness set；busy polling response queue 又浪费 CPU。

eventfd 是 Linux 提供的 64-bit counter fd：

- matching/control path 调用 `EpollServer::notify()` 写入 `uint64_t{1}`；
- eventfd readable 后，epoll 唤醒 I/O thread；
- `drain_wakeup()` 反复 read 到 `EAGAIN`；
- WakeHandler 再 drain response queue。

`notify()` 是 `noexcept`：`EINTR` retry，`EAGAIN/EWOULDBLOCK` 直接返回，因为 fd 已 readable。多个 notify 可以 coalesce；系统不依赖“一次 write 对应一次 epoll event”。

同一个 eventfd 也用于 shutdown，确保 `request_stop()` 能唤醒阻塞中的 I/O loop。

## 9. Networking self-check

1. 为什么一次 `recv()` 不能等同于一条 command？
2. 为什么一次 `send()` 不能假设写完整 response？
3. `LineFramer` 如何同时处理 split command、multiple commands 和 incomplete tail？
4. LF 和 CRLF 的处理有什么区别？max line length 如何计算？
5. 为什么 v1 选择 level-triggered epoll？
6. 为什么即使用 LT，recv/send/accept 仍循环到 `EAGAIN`？
7. 为什么 EPOLLOUT 只在 output pending 时启用？
8. `write_offset` 如何避免 partial write 后重复发送 bytes？
9. `MSG_NOSIGNAL` 防止什么 process-level failure？
10. `EPOLLRDHUP` 和 `recv()==0` 对 half-close 意味着什么？
11. 为什么 graceful close 必须等待 in-flight requests 和 pending output？
12. `close_requested` 为什么要 deferred cleanup？
13. 为什么 raw fd 不是安全的 asynchronous identity？
14. 两张 connection map 如何防止 stale response misrouting？
15. eventfd 为什么比 busy polling 更自然地融入 epoll？
16. 多次 eventfd notify 为什么允许 coalesce？
17. 当前 oversized line 在 protocol encoder 和 real network path 上分别有什么行为？
