# Matching Core 与 Event System

## 1. Core types

所有 price 和 quantity 都是 integer，不使用 floating point：

```cpp
using OrderId   = std::uint64_t;
using Price     = std::int64_t;
using Quantity  = std::int64_t;
using Timestamp = std::int64_t;
```

| Type | 当前含义 |
|---|---|
| `OrderId` | order identity；`0` 对 Add 无效。OrderBook 只拒绝与当前 active resting order 重复的 ID。 |
| `Price` | limit price；必须大于 0。具体缩放单位由调用方约定，例如可把 `10100` 理解为 101.00。 |
| `Quantity` | 可成交数量；必须大于 0；partial fill 后 Order 中保存 remaining quantity。 |
| `Timestamp` | command/order 携带的 integer timestamp；Trade 使用 taker timestamp。它不参与当前 FIFO priority 排序。 |
| `Side` | `Buy` 或 `Sell`。 |
| `OrderType` | 当前只有 `Limit`。 |

`Order` 保存 `id/side/type/price/quantity/timestamp`。`Trade` 保存 buyer ID、seller ID、execution price、per-trade quantity 和 taker timestamp。

## 2. OrderBook 的实际数据结构

```cpp
using OrderQueue = std::list<Order>;
using BidBook = std::map<Price, OrderQueue, std::greater<Price>>;
using AskBook = std::map<Price, OrderQueue, std::less<Price>>;

BidBook bids_;
AskBook asks_;
std::unordered_map<OrderId, OrderLocation> order_index_;
```

### Bid / ask price levels

- `bids_` 用 `std::greater<Price>`，所以 `begin()` 是最高 bid。
- `asks_` 用 `std::less<Price>`，所以 `begin()` 是最低 ask。
- `std::map` 保证 price level 有序，寻找 best price 不需要扫描全部订单。

### 同价 FIFO queue

每个 price level 是一个 `std::list<Order>`：

- 新 resting order 用 `push_back()` 加到队尾；
- matching 总是访问 `front()`；
- 同一 price 下先进入 OrderBook 的订单先成交。

这里的 **time priority 是 book insertion order**，不是比较 `Order::timestamp`。测试明确覆盖了“后插入订单 timestamp 更小，但仍然后成交”的情况。

### `order_index_` 和 `OrderLocation`

```cpp
struct OrderLocation {
    Side side;
    Price price;
    OrderQueue::iterator iterator;
};
```

如果只有 `std::map<Price, std::list<Order>>`，按 ID cancel 需要扫描 price levels 和订单。`order_index_` 把 `OrderId` 映射到 side、price 和稳定的 list iterator：

1. hash lookup 找到 `OrderLocation`；
2. 用 price 找到对应 bid/ask level；
3. 用 list iterator 直接 erase order；
4. level 变空时再从 `std::map` erase；
5. 最后 erase index entry。

`std::list` 的 iterator stability 正好支持这个 index design。代价是 node allocation、较差的 memory locality，以及同时维护 tree/list/hash 三种结构；v1 接受这个 tradeoff，以 correctness 和 readability 为先。

## 3. Limit order matching

### Crossing condition

Incoming BUY 可以与当前 best ask 成交，当且仅当：

```text
best_ask <= incoming_buy.limit_price
```

Incoming SELL 可以与当前 best bid 成交，当且仅当：

```text
best_bid >= incoming_sell.limit_price
```

如果下一档价格不满足 limit，就停止 matching。incoming 还有 remainder 时，rest 到自己的 limit price。

### Maker、taker 和 execution price

- 已经在 OrderBook 中的 resting order 是 **maker**。
- 新进入的 incoming order 是 **taker**。
- execution price 使用 maker/resting price，而不是 taker limit price。
- execution quantity 是 `min(taker remaining, maker remaining)`。
- `Trade::timestamp` 使用 incoming/taker 的 timestamp。

例如：book 中有 `SELL 100 x 7`，随后 `BUY 105 x 7` 进入。它可以成交，但 Trade price 是 `100`，不是 `105`。

### Partial fill 和 full fill

每笔 trade 后同时减少 maker 和 taker quantity：

- maker 变成 0：从 `order_index_` 和 price-level list 删除；
- maker 仍大于 0：继续留在 queue front，保留原来的 time priority；
- taker 变成 0：matching 结束，不 rest；
- taker 仍大于 0：继续吃下一 maker/price level；如果没有更多可成交 maker，就以 remaining quantity rest。

### Multi-level example

初始 asks：

```text
100: order 1, SELL x2
101: order 2, SELL x3
102: order 3, SELL x4
```

进入 `order 4, BUY 102 x7` 后：

```text
Trade 1: buy=4, sell=1, price=100, quantity=2
Trade 2: buy=4, sell=2, price=101, quantity=3
Trade 3: buy=4, sell=3, price=102, quantity=2
```

order 1 和 2 fully filled；order 3 remaining 2，并继续保持该 price level 的优先级；taker order 4 fully filled。

### Cancel

`cancel_order_with_result(id)` 用一次 `order_index_.find()` 同时完成两件事：

- 定位并删除 active resting order；
- 返回 cancellation 时的 remaining `Order` snapshot。

成功返回 `std::optional<Order>`；不存在的 ID 返回 `std::nullopt`，book 不变。只存在于历史 Trade、已经 fully filled 或已经 cancel 的 ID 都不再是 active order，无法再次 cancel。

## 4. Validation 与状态安全

Add 会拒绝：

- `id == 0`；
- `price <= 0`；
- `quantity <= 0`；
- unsupported `OrderType`；
- 与当前 active resting order 重复的 ID。

这些 validation 在 matching 前完成，失败时抛出 `std::invalid_argument`，不改变 book，也不生成 Event。

当前实现没有对 `Timestamp` 做业务范围校验，也没有全局历史 ID registry。文档和调用方不能把这些未实现的规则当成 v1 guarantee。

## 5. Event model

```cpp
using EventPayload = std::variant<
    OrderAccepted,
    OrderCancelled,
    TradeCreated,
    OrderFilled,
    OrderPartiallyFilled>;
```

| Event | 表达的事实 |
|---|---|
| `OrderAccepted` | Add 已通过 validation 并完成 matching 调用；保存 submitted order 的原始 quantity。 |
| `OrderCancelled` | active order 已删除；保存 cancellation 时的 remaining Order state。 |
| `TradeCreated` | 一笔具体成交已发生。 |
| `OrderFilled` | 该笔 trade 后，该 order remaining quantity 为 0。 |
| `OrderPartiallyFilled` | 该笔 trade 后，该 order 仍有 remaining quantity。 |

`OrderFilled::filled_quantity` 和 `OrderPartiallyFilled::filled_quantity` 都表示 **当前这笔 Trade 的 quantity**，不是 cumulative filled quantity。

### Deterministic event ordering

一次 successful Add 的顺序固定为：

```text
OrderAccepted(taker)

for each Trade in matching order:
    TradeCreated
    maker OrderFilled / OrderPartiallyFilled
    taker OrderFilled / OrderPartiallyFilled
```

一次 successful Cancel 只发布一个 `OrderCancelled`。Failed Cancel 不发布 Event。

以前面的 multi-level 例子为基础，如果 taker quantity 改成 4：

```text
OrderAccepted(order 4, original quantity 4)
TradeCreated(order 4 vs order 1, quantity 2 @ 100)
OrderFilled(order 1, filled_quantity 2)
OrderPartiallyFilled(order 4, filled_quantity 2, remaining 2)
TradeCreated(order 4 vs order 2, quantity 2 @ 101)
OrderPartiallyFilled(order 2, filled_quantity 2, remaining 1)
OrderFilled(order 4, filled_quantity 2)
```

这条顺序让 consumer 不需要读取 mutable OrderBook，也能按顺序理解每笔成交和双方状态变化。

## 6. 为什么 Event 不放进 OrderBook

`OrderBook` 的职责是维护 matching-state truth。它返回 `Trade` 和生成 Event 所需的最小 execution state，但不知道 EventCollector、network protocol 或 consumer。

`MatchingEngine` 是 application-level wrapper：

```text
OrderBook:      state transition + Trade facts
MatchingEngine: Trade facts -> externally consumable Events
EventCollector: ordered in-memory storage
```

这样分层有几个直接结果：

- Core benchmark 可以直接调用 OrderBook，尽量排除 Event overhead；
- Event semantics 可以演进，而不把 consumer-specific logic 塞进 matching loops；
- Replay 和 TCP Gateway 可以复用同一 MatchingEngine behavior；
- OrderBook 保持唯一 state owner，不在上层维护 duplicated order state。

## 7. 当前 detailed result APIs 的工程理由

### `AddOrderExecutionResult`

OrderBook 提供：

```cpp
struct AddOrderExecutionResult {
    std::vector<Trade> trades;
    Quantity last_trade_maker_remaining_quantity{};
};
```

`add_order()` 和 `add_order_with_execution_result()` 共用唯一的 `execute_order()` matching implementation。原 API 仍返回 `std::vector<Trade>`；MatchingEngine 使用 detailed result。

在一个 add operation 中，早于最后一笔 Trade 的 maker 必须已经 fully filled，否则 matching 不会前进到下一 maker。因此只需返回 `last_trade_maker_remaining_quantity`：

- earlier makers 的 remaining 必然是 0；
- final maker 可能是 0，也可能 partial；
- MatchingEngine 不必为每个 Trade 再调用 `OrderBook::find_order(maker_id)`。

这既减少 redundant lookup，也保持 OrderBook 是 matching-state truth owner；`Trade` 和 `Event` layouts 没有改变。

### `cancel_order_with_result()`

`bool cancel_order()` 和 `cancel_order_with_result()` 共用 `cancel_order_impl()`。Detailed API 让 MatchingEngine 从真正执行 cancel 的同一次 lookup 得到 Order snapshot，不需要先 `find_order()` 再 `cancel_order()`。

API 表达的是实际 execution result，而不是在 MatchingEngine 中缓存 duplicated state。即使 benchmark 对这一变化没有显示 material throughput improvement，这仍是更准确、更干净的 ownership boundary。

## 8. Complexity 与 tradeoffs

用当前 STL design 粗略分析：

- best bid/ask：`O(1)` 访问 map begin；
- 新 price level 插入：`O(log P)`，`P` 是 price-level 数；
- 同价 order append：找到/创建 level 后 list append 为 `O(1)`；
- 每个被成交 order 的 index erase：unordered_map average `O(1)`；
- cancel index lookup：average `O(1)`，price-level lookup `O(log P)`，list erase `O(1)`；
- matching 总成本还取决于实际跨过的 price levels 和 makers 数量。

这些是 expected/structural complexity，不是 hard real-time guarantee。`std::unordered_map` worst case、dynamic allocation 和 node-based containers 都仍然存在。

## 9. What you should be able to explain yourself

读完后，应当可以不看代码解释：

1. 为什么 bids 使用 descending map，而 asks 使用 ascending map？
2. 为什么 time priority 来自 list insertion order，而不是 `Timestamp`？
3. 为什么 maker price 是 execution price？
4. 一个 taker 如何跨多个 price levels 产生多笔 Trade？
5. maker partial fill 后为什么仍保留原有 queue priority？
6. 为什么同时需要 ordered price levels、FIFO list 和 `order_index_`？
7. `OrderLocation` 为什么保存 list iterator？它依赖什么 iterator-stability property？
8. `filled_quantity` 为什么是 per-trade，而不是 cumulative？
9. 为什么 Event 顺序是 Trade、maker state、taker state？
10. OrderBook state 和 Event stream 分别解决什么问题？
11. 为什么 `last_trade_maker_remaining_quantity` 足够描述 maker state？
12. Detailed cancel API 如何在不复制状态的情况下避免 duplicate lookup？
13. 当前 design 的主要 readability 优势和 memory-locality 代价是什么？
14. 给定相同 command sequence，哪些部分保证 matching deterministic？
