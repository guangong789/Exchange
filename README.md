# Agent Economy Exchange

本项目以已有的 C++ Exchange core 为确定性经济基础，在其上组合 Analyst、Risk 与 Trader Agent；撮合与记账规则决定最终经济状态，Binance/x402 提供外部支付能力边界。**Agent 决策不直接成为经济事实。**

## 1. 这个项目是什么？为什么用自己的 Exchange core 作为 Agent 项目的基础？

项目最初是一套 C++20 确定性撮合与记账核心，Agent、模型、支付和 UI 都是后来加入的组合层。MatchingEngine、ExecutionCoordinator、AccountStore 与 Ledger 不感知 DeepSeek、Binance Agentic Wallet 或前端。

会产生经济变更的 Agent action 必须经过经济校验；订单类 action 还需要经过资金预留与撮合，才能形成可 Replay 的经济结果。BUY、SELL 与 CANCEL 受资金和市场规则约束；HOLD 是一等 action，但不会产生经济状态变更。

当前实现是单机、内存内的工程项目，不是生产级交易所，也不提供持久化、灾难恢复或 ACID 保证。移除 `demo-ui/`、DeepSeek、x402 与 Binance 后，**确定性经济核心仍可独立构建和测试。**

## 2. 三个 Agent 怎么协作？x402 和 Binance Agentic Wallet 在这里做什么？

### 权限与职责边界

- **Analyst Agent**：提供市场信号与服务信息。
- **Risk Agent**：根据资金、盘口、风险预算与目标给出确定性数量上限。
- **Trader Agent · DeepSeek**：拥有最终 action 决策权。
- **ExecutionCoordinator**：决定 action 能否成为经济事实。

Analyst 提供信息，Risk 提供约束，Trader 决策，经济核心执行或拒绝：Analyst → Risk → Trader · DeepSeek → AgentAction → 确定性经济核心。

当前多轮场景中，Analyst 给出 BUY_BASE，Risk 上限随余额、盘口与目标变化；Trader 可以 BUY 或合法地 HOLD，上一轮经济结果会进入下一轮观察状态。

### 两条 x402 路径

Society Simulation 使用确定性的 PreviewAuthorized 保持多轮场景可重复，不执行链上结算。

**x402 已经到达 Binance Agentic Wallet 的支付预览边界。** Live x402 Evidence 是独立、可选的外部集成：后端构造 BSC（eip155:56）上的 USDT/Tether USD 支付要求，调用 Wallet CLI 并规范化 Provider 返回。

当前 Provider 返回：

- Wallet CONNECTED · Provider binance-agentic-wallet
- Preview **ACTION_REQUIRED** · Reason **INSUFFICIENT_BALANCE**
- BSC / eip155:56 · USDT 10000 原子单位 · 资金移动 0

**Settlement 未执行。** 当前没有签名、广播或资金转移，服务保持 locked。

## 3. 为什么不能让 Agent 直接改余额？Matching / Accounting / Ledger 有什么价值？

经济执行链路是：AgentAction → AgentActionGateway → ExecutionCoordinator → MatchingEngine → Account / Reservation / Ledger。

**所有可执行订单都有对应的账户资金支持。** 买单预留 quote，卖单预留 base；Price-Time Priority 决定 maker，并以挂单价格生成 Trade。ExecutionCoordinator 协调订单准入、资金预留、余额变更与 Ledger。

部分成交直接体现请求数量与成交数量的区别：

```text
BUY 2 @ 101
→ executed 1 @ 101
→ remaining BUY 1 @ 101 rests
→ 101 QUOTE remains reserved
```

**部分成交后，剩余资金继续处于 reserved 状态。** Trade 记录实际成交，剩余挂单仍由对应预留资金支持。

当前 episode 证据：

- 执行：4 张 taker order · 4 条 Trade · 成交 5 BASE
- 资金：执行 507 QUOTE · 预留 101 QUOTE
- 审计：15 条 Ledger · 无效状态变更 0

Trade 条数不等于成交数量；Ledger 是内存内审计镜像，不是数据库或外部结算系统。

本次 5-round episode 的 Score 为 63。Score 仅用于 Society Simulation 的行为评估，不属于余额、Ledger 或 Replay economic parity。

## 4. Replay 为什么值得单独设计？

**Replay 重建的是经济状态，不是 UI，也不是 DeepSeek 的决策过程。** 它只接收原始 episode 中已进入经济边界的确定性输入。

```text
Original episode
  DeepSeek + Analyst service + Risk / Agent logic
                         │
                         ▼
           captured deterministic economic inputs
                         │
                         ▼
Replay                   fresh deterministic world
  DeepSeek calls = 0               │
  Analyst calls  = 0               ▼
  x402 / Wallet  = 0      ExecutionCoordinator
                                  ▼
                           MatchingEngine
                                  ▼
                    Account / Reservation / Ledger
                                  ▼
                       exact state comparison
```

**Replay 在 fresh world 中重建相同经济结果。** 当前 episode 有 5 次 DeepSeek 决策、5 次 Analyst 服务访问和 4 个已捕获经济输入；其中一轮是 HOLD，因此发生了 cognition，但没有需要捕获的经济 action。

- DeepSeek calls：5 → 0
- Analyst service accesses：5 → 0
- Economic inputs：4 → 4

4 个输入通过同一个 ExecutionCoordinator、MatchingEngine、Account、Reservation 与 Ledger 路径执行：

- Trader BASE 可用余额：5 / 5 · **EXACT**
- QUOTE 可用 / 预留：532 / 532 · 101 / 101 · **EXACT**
- Trade / Ledger 条数：4 / 4 · 15 / 15 · **EXACT**
- Objective：5/5 / 5/5 · **EXACT**

**经济输入不等于模型决策历史。** 外部决策可以具有概率性，但越过经济边界的输入及其后果可以确定性重建；余额、Ledger、订单、Trade 与目标均为 EXACT。

## 5. 这套架构可以如何演进？

### 今天已经实现

- 独立于 Agent 的撮合与记账基础：Price-Time Priority、部分成交、撤单、account-backed 预留、同步清算与 Ledger 审计镜像。
- Analyst、Risk、Trader 三角色协作；DeepSeek 通过 ModelAdapter 参与 Trader 决策，HOLD 是一等 action。
- localhost x402 付费服务语义，以及 Binance Agentic Wallet 的可选支付预览证据。
- 捕获经济输入、重建 fresh world 并精确比较状态的 **Deterministic Replay**。
- Linux epoll TCP gateway、有界队列、单撮合线程、协议测试与 benchmark 工具。

### 下一步可以自然扩展

该架构可演进为 Agent-to-Agent 服务与信息市场：Agent 购买服务，在私有目标与约束下决策，再由确定性设施约束后果。Wallet-backed Agent、可替换模型 Provider、reputation/contracts、结算适配器与长期运行的 Agent society 均属于后续方向；当前没有自动支付、链上结算或持久化 reputation。

## 架构一览

- `matching/` + `accounting/`：撮合、资金预留、余额与 Ledger
- `agent/` + `arena/`：Agent 身份、观察、动作与多 Agent 场景
- `model/`：模型抽象与 DeepSeek 适配
- `x402/` + `binance/`：付费服务边界与 Binance Agentic Wallet 预览
- `hackathon/` + `demo-ui/`：Society 场景编排与演示界面
- `replay/`：确定性经济重建
- `gateway/`：Linux epoll 网络 I/O、有界队列与单撮合线程

**确定性经济核心不依赖 Agent、DeepSeek、Binance 或 UI。** 上层通过组合扩展能力，外部模型与支付 Provider 的语义不会进入撮合和记账核心。

## 运行演示

完整演示路径面向 Linux / WSL2，需要 C++20 编译器、CMake 3.20+、libcurl 与 nlohmann-json。

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DEXCHANGE_BUILD_TESTS=ON
cmake --build build -j
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
export DEEPSEEK_API_KEY='<process-local key>'
python3 demo-ui/server.py --port 8765
```

访问 http://127.0.0.1:8765。API key 不应写入仓库、前端代码或测试数据；离线测试不需要该 key。

启用 Live x402 Evidence 前需要：

1. Node.js 20+。
2. 安装 @binance/agentic-wallet，完成 Wallet 认证。
3. 确认 baw wallet status --json 返回可用的连接状态。
4. 让 UI bridge 进程能够找到 baw；必要时设置 EXCHANGE_BINANCE_WALLET_CLI。

```bash
export EXCHANGE_BINANCE_WALLET_CLI="$(command -v baw)"
python3 demo-ui/server.py --port 8765
```

Live x402 Evidence 是可选外部集成，不属于默认离线测试。`.tools/` 用于本机 CLI 工具并保持未跟踪；Wallet session、token 或凭据不得提交。

## 验证

现有 build/ 配置的完整离线测试集：

```text
428 / 428 tests passed
```

默认测试集覆盖撮合、记账、Agent/Arena、Replay、gateway、x402 与 Binance Wallet 适配层，不调用 DeepSeek 或 Binance 外部服务；外部调用测试由 CMake 选项显式开启。
