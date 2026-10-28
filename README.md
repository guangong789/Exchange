# Deterministic Agent Execution Runtime

简体中文 | [English](README-EN.md)

一个使用 C++20 构建的确定性交易与 Agent 执行系统，支持账户交易、双边合约和内部结算，通过同步 WAL 与崩溃恢复重建权威状态。脚本或规则 Agent 提出类型化动作，Python AI 诊断基于执行证据生成解释；执行与事实判断都受确定性机制约束。

**Agent 动作与 AI 诊断都受确定性机制约束，不能自行成为权威状态或可信结论。**

## 架构

```mermaid
flowchart TB
    subgraph execution_path["执行 · 权威状态变更"]
        direction LR
        client["客户端 /<br/>类型化 Agent 动作"] --> admission["确定性准入"]
        admission -->|持久化| wal["同步 WAL"]
        admission -->|非持久化| apply["交易 / 合约执行"]
        wal --> apply
        apply --> state["账户 / 订单<br/>合约 / Ledger"]
    end

    subgraph diagnostics_path["诊断 · 只读证据解释"]
        direction LR
        claims["用户问题 / 模型声明"] --> validation["Schema / 证据校验"]
        evidence["C++ 证据<br/>执行快照 / 离线 WAL 重放"] --> validation
        validation --> summary["本地生成的可信摘要"]
    end

    execution_path ~~~ diagnostics_path

    classDef intent fill:#fff0df,stroke:#bf762e,color:#754213
    classDef execution fill:#e8f0fc,stroke:#6486b5,color:#203e65
    classDef state fill:#284f7c,stroke:#1c3d62,color:#ffffff
    classDef evidence fill:#edf7ef,stroke:#7ba98b,color:#2c5438
    classDef trusted fill:#d9eeea,stroke:#398579,color:#20574e
    class client,claims intent
    class admission,wal,apply execution
    class state state
    class evidence evidence
    class validation,summary trusted
```

TCP I/O 可以并发，权威状态修改保持串行；Agent 也可以直接调用 runtime，不必经过 TCP。持久化模式在应用命令前完成 WAL 同步。诊断单独读取证据，不修改权威状态。

## 设计取舍

### Q1. 为什么并发处理网络请求，却串行执行状态变更？

一笔交易同时涉及订单簿、账户预留和 Ledger；让这些修改交错执行，会增加中间状态和跨模块协调的复杂度。因此 gateway 用 epoll 处理并发 I/O，通过有界命令和响应队列连接单个执行线程，并按连接身份路由响应。有界队列限制积压，明确的命令顺序则让状态变化与重放更容易推理。

确定性针对相同初始状态和有序命令，不保证并发请求每次以相同顺序到达。直接调用 Agent/runtime 接口也必须遵守串行执行约定。这让权威状态拥有明确的执行顺序，使账户一致性和故障恢复更容易推理。

### Q2. 如何保证成交、资金预留与结算结果一致？

撮合引擎能决定谁与谁成交，却不能独自保证账户有钱履约。账户执行路径先为买单预留报价资产、为卖单预留基础资产，再通过财务预检查核对预计成交的资金需求。撮合返回后，实际 trades 必须与计划一致，随后驱动资产清算和按资产平衡的 Ledger 记账。

部分成交保留未完成部分的预留；买单获得价格改善时释放多余预留；撤单验证所有权，只释放剩余义务。双边合约结算则只转移可用报价资产，不能花掉订单已经预留的资金；这些行为由[账户执行测试](tests/accounting/execution_coordinator_test.cpp)和[合约结算测试](tests/durability/durable_contract_execution_test.cpp)覆盖。

每个账户、每种资产都保持 `available + reserved <= INT64_MAX`，入账和转账也计入已有预留，从而保证预留资金可以释放。结算入账会超出此范围的订单在撮合前返回 `INVALID_ORDER`，余额、预留、订单簿和 Ledger 保持不变。持久化模式仍记录这条已准入命令并消耗执行身份，恢复时重现相同拒绝；恰好达到上限的合法余额仍可使用。

### Q3. 如果命令已持久化，但客户端没有收到响应，会发生什么？

持久化模式先追加命令并同步 WAL，再应用状态变更，然后返回业务响应。进程可能在执行后、响应送达前崩溃，连接也可能在这个窗口断开。重启会校验日志，并从匹配的初始账户配置按顺序重放有效历史，重建订单、余额、预留、合约和 Ledger。因此，客户端没有收到响应，不能推断命令未执行；[真实进程测试](tests/gateway/durable_server_process_test.cpp)覆盖了这一窗口。

WAL 接收也不等于业务成功：资金不足等交易拒绝可能发生在写入之后，并在恢复时重现。RequestId 仅用于关联，相同值再次提交仍可能创建新订单，没有通用去重或 exactly-once 保证。已结算合约会拒绝再次结算以防止重复付款，这是合约状态机的局部保护。

意外的 journal/application 异常会原样传播，并使交易和合约执行共同进入 poisoned 状态，非持久化交易也遵守这一边界。局部插入回滚和撮合前回滚不代表撮合或事件发布开始后的全局回滚；后续执行必须丢弃该 runtime，或从 WAL 创建恢复实例。WAL 编码没有改变；包含旧余额溢出缺陷的历史按修正后的财务规则重放为业务拒绝，不再重建无法释放预留的余额。

### Q4. Agent 提出的交易动作如何受到执行约束？

`AgentDecisionProvider` 返回封闭的类型化动作集合；当前保留脚本和规则策略。AgentRuntime 检查结构和配置的硬经济约束，再由宿主侧 adapter 从注册表绑定账户与行动者身份。动作字段不能将请求切换到其他执行账户。

动作随后进入实际执行路径，继续检查余额、撤单所有权、合约参与权限和状态转换。Agent 接口只提交动作请求，账户、订单簿和 Ledger 由执行层修改。经济 profile 提供额外的准入约束，TCP 客户端不会自动继承它；最终是否执行由执行层的业务检查决定。

### Q5. 为什么模型输出了合法 JSON，诊断结论仍然不能直接相信？

合法 JSON 只说明格式可解析，schema 校验也不能证明事实正确。诊断入口还要将受支持的事实和原因与 C++ 执行证据核对，并从已验证字段在本地生成可信摘要；即使结构化事实全部正确，模型附带的任意文字仍不可信。例如，部分成交订单的余量仍挂在簿上，模型文字却说“已撤销”，本地摘要会报告已验证的挂单状态；如果结构化事实本身也声称已撤销，则拒绝诊断。[诊断回归测试](diagnostics/python/tests/test_diagnostic_runner.py)验证了原文不能改变可信摘要。

证据范围也决定能回答什么：执行时 partial-fill 证据来自固定的真实 C++ 场景，不支持任意历史上下文查询；历史工具在离线 WAL 私有副本上运行生产恢复，结论限于恢复前缀。历史证据不能证明原始响应送达，也不能补出缺失的执行前撮合上下文或任意停止撮合原因。两个独立工具中相同的 OrderId 不代表同一次执行。

### Q6. 为什么优化了 Ledger，持久化吞吐却几乎没有变化？

此前 profiling 将开销定位到 Ledger 历史增长中的反复搬移：小批次不断精确预留所需容量，会频繁复制或移动已有记录。[实现](src/accounting/ledger.cpp)改为有上限的几何扩容，减少搬移次数，代价是额外空余容量。分配仍发生在 prepare 阶段，commit 保持无分配且 `noexcept`，记账和 WAL 语义未改变。

既有同环境配对测量中，4 客户端、10,000 条命令的非持久化 gateway 吞吐从 **21,204 → 51,550 commands/s（约 2.43×）**；4 客户端、2,000 条命令的持久化吞吐则为 **250.32 → 250.28 commands/s**。环境为 GCC 13.4、Release `-O3`、WSL2 / i9-12900H。在上述非持久化负载中，减少 Ledger 历史搬移带来了明显收益；持久化负载则仍由逐命令 WAL 同步主导。

## 快速开始

Linux / WSL2 需要 C++20 编译器、CMake 3.20+、nlohmann-json CMake package。构建测试时还需 GoogleTest（未安装时 CMake 通过 FetchContent 下载）；脚本默认模式关闭测试。nlohmann-json 用于保留的 C++ 证据工具；C++ 构建不需要 libcurl、OpenSSL 或 Boost。

无需实时模型或外部 API，[主 demo](apps/exchange_runtime_demo/main.cpp)使用脚本动作和真实 WAL，展示交易、合约结算、响应交付前的进程崩溃与精确恢复。在仓库根目录运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/exchange_runtime_demo
```

也可运行 `./quickstart.sh` 构建并演示，或使用 `./quickstart.sh test` 运行完整 C++ 测试。

主 demo 使用本地脚本，不调用行情或模型 API。Python AI 诊断及其独立的模型 provider 仍保留。Python 诊断测试需 Python 3.10+：`python3 -m unittest discover -s diagnostics/python/tests -v`；默认不调用实时 API，显式 opt-in 的历史 C++ 集成测试默认跳过。

## 范围与限制

系统在单节点上串行执行权威状态变更，每个 runtime 对应一个交易品种，使用内部账户余额和结算，提供明确的持久化、恢复和 AI 接入边界。项目不提供多节点 HA 或通用 exactly-once，不路由真实订单、不托管资产，也不是生产交易所或通用 Agent 编排平台。
