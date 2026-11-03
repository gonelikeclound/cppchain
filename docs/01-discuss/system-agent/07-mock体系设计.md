# 07 · Mock 体系设计：怎么测一个"编排一个不存在的世界"的系统

> 日期：2026-10-29
> 前置：[`02`](./02-任务拓扑图与两层上下文-架构草案.md)–[`06`](./06-case-C6-事件洪水推演.md)
> 目标：给 SA 做一套**可重复、可加速、可注入故障、可度量成本**的测试基座，让 C1/C6/C7/C10/C11 能真跑起来。

---

## 0. 先说结论

**不要从"假设备"开始，要从"验证器直灌"开始。** 理由：SA 的运行时契约是
"给定一个 patch，系统是否仍然安全"——这件事**不需要 LLM、不需要世界、不需要时钟**，
纯函数就能测。而它是所有其它测试的地基。

---

## 1. 结论先行

| # | 结论 |
|---|---|
| **R1** | DSH 已有 4 件可直接复用（`llm-mock-server` / `llm-replay` / `agent-loop-testkit` / `invariants`），SA 只需新建 4 件。**别重造。** |
| **R2** | **机制测试不经过自然语言**：用 `patch` 直灌测运行时，用真模型测编排质量。两者混在一起就既慢又不稳。 |
| **R3** | **必须虚拟时钟，但只虚拟"系统时钟"**：定时器/超时/窗口/退避走虚拟，模型与 IO 走真实。混成一个必然 flaky。 |
| **R4** | **断言查世界，不查返回值**。Mock 必须能"说谎"（返回成功但世界没变）——否则测不出结果真实性。 |
| **R5** | **CI 里绝不允许真模型；质量评估里绝不允许假模型。** 两类断言必须物理隔离。 |
| **R6** | 先测**坏模型**（对抗、循环、说谎、超预算），最后才测好模型。运行时的价值在于"编排者错了也安全"。 |
| **R7** | 分五期落地，**M0 只要 2~3 天**且不需要任何 mock 基建。 |

---

## 2. 复用清单：DSH 已经有的

先看清手牌（本项目 checkout 里已存在，非虚构）：

| 已有件 | 能力 | SA 怎么用 |
|---|---|---|
| **`llm-mock-server`** | 可脚本化 OpenAI 兼容 HTTP/SSE **故障服务器**；按到达顺序消费行为（`--sequence partial_disconnect,success`）；带 `randomSeed`、请求捕获、telemetry | **对抗模型**的主力：半截流、超时、5xx、慢响应 |
| **`llm-replay`** | 回放录制好的 session JSONL；支持 sidecar 覆盖、**`hang` 条目 + `readyFile` 标记**（可确定性触发取消）、`{{fromRequest:regex}}` 占位符 | **录制回放**形态；特别是"挂起后精确取消"的测试 |
| **`agent-loop-testkit`** | `mountAgentLoopTestDependencies` 按依赖序挂载 LLM/session/prompt/tool/agent 服务 | 把 SA 插件挂在真实 AgentLoop 上做集成测试 |
| **`invariants`** | 开发期运行时不变量断言 | **机制断言的落点**：把 V1–V8、D-a~D-c 写成 invariants |

## 3. 新建清单：SA 专属的四件

| # | 组件 | 为什么 DSH 没有 | 工作量 |
|---|---|---|---|
| **N1** | **虚拟时钟 + 确定性调度器** | DSH 用真实 timers；一夜 8 小时没法真跑 | 中 |
| **N2** | **世界模型（World）** | 需要一个"可被检查、可被说谎"的假世界 | 中 |
| **N3** | **有状态能力 mock（假电话 FSM）** | 没有 stateful 能力的模拟器 | 小～中 |
| **N4** | **场景 DSL + 断言运行器 + 度量采集** | 用例需要是**数据**而非代码 | 中 |

---

## 4. 五个可替换面

SA 的依赖面很窄，这是它的运气。只需替换这五处：

```
        ┌──────────── 场景 DSL（N4）────────────┐
        │                                        │
  [事件源]──► 漏斗 ──► [编排 LLM] ──► patch ──► [图运行时]
        │              ▲                        │
        │         [模型 mock]                   │
        │              │                        ▼
  [时钟 N1]◄───────────┴──────────► [能力 mock N3] ──► [世界 N2]
                                                      ▲
                                              断言只查这里
```

| 面 | 替换成 | 保留真实的部分 |
|---|---|---|
| 事件源 | 生成器（脚本/录制/洪峰） | 事件类型与 schema 真实 |
| 模型 | 4 种 mock 形态（§5） | 适配器与流式协议真实（走 `llm-mock-server`） |
| 能力 | `MockStatelessCap` / `MockStatefulCap` | 工具契约、权限、预算真实 |
| 时钟 | 虚拟（系统域） | 不需要真实等待 |
| 世界 | 内存世界 + 只追加副作用账本 | 不需要真设备 |

**图运行时不被 mock。** 它是被测对象。

---

## 5. 模型 mock 的四种形态（最关键的一节）

| 形态 | 实现 | 用途 | 稳定性 |
|---|---|---|---|
| **① scripted**（剧本） | `llm-mock-server --sequence` | 特定失败路径：半截流、重试、超时 | 高 |
| **② recorded**（录制回放） | `llm-replay` + session JSONL | 回归：真实模型曾产生过的行为 | 高 |
| **③ adversarial**（对抗） | `llm-mock-server` 行为库 | 说谎/循环/幻觉工具/超预算 | 高 |
| **④ oracle-patch**（直灌补丁） | **新建，最简单** | **机制测试主力** | 最高 |

### 5.1 为什么必须有 `oracle-patch`

机制测试问的是："**给定这个 patch，系统是否安全？**"

这件事和"模型怎么想出这个 patch"**完全无关**。所以：

```ts
// 不经过任何自然语言
const script: PatchScript = [
  { at: 0,    patches: [addNode(callSpec), addNode(assertSpec)] },
  { at: 'rejected:1', patches: [] },                    // 拒接：不唤醒（策略内重试）
  { at: 'ended:user_redirected',
              patches: [addNode(messageSpec), rewire('n_call','n_assert')] },
];
```

- 结果是**完全确定**的，可以断言到每一次 patch、每一次验证、每一次唤醒；
- 不依赖 prompt 稳定性 → **prompt 改了不会导致 50 个测试变红**；
- 跑得极快（无 HTTP、无 token）。

> **建议：把所有"运行时正确性"的验收都建在 ④ 上，把 ①②③ 用于适配器与恢复策略，把真模型留给质量评估。**

### 5.2 对抗形态要覆盖的坏行为

| 坏行为 | 为什么必须测 | 期望系统反应 |
|---|---|---|
| 声称成功但没做 | 结果真实性第③支柱 | 断言失败 → `Incomplete` |
| 重复调用同一工具同参数 | ReAct 循环的典型症状 | S4 撞墙信号 → 升级/阻断 |
| 幻觉出不存在的工具 | 编排层最常见的错 | 验证器拒绝 patch |
| 产出非法 patch（悬空边、环） | 图完整性 | V1/V3 拒绝 + 回滚 |
| 一次产出上百个 patch | 改图爆炸 | patch 上限 K → 拒绝 |
| 超出预算仍要新资源 | 成本失控 | V4 拒绝 |
| 删掉不该删的节点 | 自杀式完成 | digest 的 `protected` 保护 + V7 |

---

## 6. 四个自建组件的设计要点

### 6.1 N1 虚拟时钟：**双时钟域**

```
系统时钟（虚拟）  定时器 / 超时 / 关联窗口 / 重试退避 / 打断超时
模型时钟（真实）  HTTP、流式、mock server
```

**桥接规则（关键，否则必然 flaky）**：

1. 虚拟时钟只在**系统静默**时推进（`quiescence-then-advance`）：先排空所有 microtask 与已就绪的虚拟定时器，确认无进展，再跳到下一个定时器时刻。
2. IO 完成**不推进**虚拟时钟；等待 IO 时若有更早的虚拟定时器到期，允许显式 `advance_to(timer)`。
3. 事件调度的全序：`(virtual_time, priority, seq)`。**seq 单调**是确定性回放的前提。
4. 被测代码**禁止直接使用** `Date.now()` / `setTimeout`（lint 强制），一律走 `Clock` 服务。

**验收**：同一 case 连跑 100 次，事件序与终态**逐字节一致**。

### 6.2 N2 世界模型：三种诚实度

```ts
interface World {
  fs: VirtualFs
  apps: Map<string, {version, state: 'absent'|'installing'|'installed'|'partial'}>
  calls: Map<CallId, {state, transcript, started_at}>
  messages: Array<{to, body, delivered_at}>
  battery: number; network: NetworkState; location: GeoContext
  sideEffects: SideEffect[]   // 只追加账本
}
```

**三种诚实度**（这是本组件存在的全部理由）：

| 模式 | 行为 | 用来测什么 |
|---|---|---|
| `honest` | 返回成功 ⇔ 世界真的变了 | 基线 |
| **`lying`** | **返回成功但世界没变** | 结果真实性：断言必须能抓住 |
| `partial` | 世界变了一半（安装到 60%） | 对账 / 善后 / 隔离 |

> **纪律：所有断言只查 `World`，永不查工具返回值。**
> 这一条把"高可信第③支柱"变成了可执行的测试规则，而不是口号。

### 6.3 N3 假电话：一个有真实 FSM 的 mock 能力

```yaml
capability: make_call
kind: stateful
fsm: phone            # Dialing→Ringing→Connected→Talking→Ended | Failed
script:
  - {at: 4s,   state: ringing}
  - {at: 22s,  action: reject}
  - {retry: 1, at: 6s, state: ringing}
  - {retry: 1, at: 12s, action: answer}
  - {at: 120s, action: barge_in, intent: redirect_to_messaging}
behaviors:
  ignore_sigterm: false        # 置 true 用于 C10 卡死用例
  hangup_delay_ms: 300
  transcript: canned
```

要点：

- **在虚拟时钟上跑**，所以"通话 146 秒"是瞬时完成的；
- 支持 `barge_in`（模拟用户中途改口，直接打给控制平面）；
- 支持 `ignore_sigterm`（模拟"取消了却退不出"）；
- 支持 `partial` 世界（通话建立到一半链路断）。

**这一个 mock 就能覆盖 C7 的全部路径**，不需要真电话。

### 6.4 N4 场景 DSL：case 是数据，不是代码

```yaml
case: C7-phone-reject-then-redirect
clock: virtual
world:
  contacts: [妈妈]
  capabilities: {make_call: {uses: phone-script-v1}}
llm: {mode: oracle-patch, script: c7.patches.json}
events:
  - {at: 0s,   user: 给妈妈打个电话问她晚上来不来吃饭}
  - {at: 120s, user_speech: 算了，发个微信就行}
inject:
  - {kind: event_storm, at: 300s, count: 4000}     # C6 复用同一 DSL
assert:
  - {model_calls: '<= 3'}
  - {world.calls[0].state: ended}
  - {world.messages: [{to: 妈妈, delivered: true}]}
  - {graph.final: Completed}
  - {milestones_seen: [call.rejected, call.answered, call.ended]}
  - {invariants: [V1..V8, I0, I1]}
metrics: [model_calls_by_layer, wakeups, patches, verify_rejects, cancellations]
```

**为什么必须数据化**：可 diff、可回放、可批量生成、可做覆盖率统计。
case 写成测试代码的那一刻，它就失去了这四种能力。

---

## 7. 故障注入清单

| # | 故障 | 对应 case | 期望 |
|---|---|---|---|
| F1 | 任意 step 边界 `kill -9` | 全部 | 重启后终态与对照组**逐字段一致**（crash-only） |
| F2 | 事件重复投递 | 全部 | 幂等键丢弃 + 记 `DUPLICATE_DROPPED` |
| F3 | 事件乱序 | C6 | per-key 串行后终态唯一 |
| F4 | 事件洪峰 10× / 100× | C6 | L0-a 延迟不劣化；降级到白名单模式 |
| F5 | 节点忽略 SIGTERM | C10 | 四级升级走到 abandon → 派清理任务 |
| F6 | 模型说谎 | C1/C3 | 断言失败 → `Incomplete` |
| F7 | 模型循环 / 幻觉工具 | C1 | S4/S5 撞墙 → 升级或阻断 |
| F8 | 系统时间跳变 | C5 | 窗口/退避不崩（虚拟时钟应天然免疫） |
| F9 | 日志写失败 | 全部 | **失败关闭**（fail-closed），不得降级为"继续跑" |

---

## 8. 分五期落地（先做最便宜的）

| 期 | 内容 | 依赖 | 产出 | 工期 |
|---|---|---|---|---|
| **M0** | **patch 直灌 + 内存图 + 验证器** | **无** | V1–V8 全部可测；非法 patch 全部被拒 | **2~3 天** |
| **M1** | 虚拟时钟 + 世界模型 + 假电话 + DSL | N1/N2/N3/N4 | C1、C7 端到端 | 1~1.5 周 |
| **M2** | 故障注入（F1–F9）+ 洪峰 | M1 | C6、C10、C11 | 1 周 |
| **M3** | 录制回放 + 真模型抽样 | `llm-replay` | 编排质量校准（非阻断） | 3~5 天 |
| **M4** | 真机影子（可选） | — | 真实设备抽样，校准 mock 保真度 | — |

**M0 的价值最高**：验证器是纯函数，不需要任何基建，却能立刻把"图是否安全"这件事锁死。
如果 M0 就发现设计漏洞（很可能——比如 V2 的可判定性问题），后面三期都能省。

---

## 9. 度量点 → 验收口径映射

采集器只需要读**审计事件流**（SA 本来就是事件溯源架构，这是它的红利）：

| 度量 | 来源事件 | 对应验收 |
|---|---|---|
| `model_calls_by_layer` | LLM 调用审计 | V-a、V-1（C1 ≤1、夜 ≤5） |
| `wakeups` | L1b 唤醒 | 成本模型 |
| `patches` / `verify_rejects` | patch 事件 | D2、V1–V8 |
| `escalations` | 撞墙事件 | S1–S8、升级 ≤1 次 |
| `cancellation_levels` | 四级升级 | C10、`abandon` 计数 |
| `orphan_resources` | 资源账本差集 | 善后完整性 |
| `miss_rate` | 影子通道 | R4 漏报审计 |
| `l1_size` | L1 快照 | Q7 有界性 |
| `cancelling_ms` p99 | 僵尸检测 | §8.5 |
| `interruptions` | 用户触达 | 打扰预算 |

---

## 10. 六个陷阱

| # | 陷阱 | 后果 | 规避 |
|---|---|---|---|
| 1 | **mock 太深**（把图运行时也 mock 了） | 测的是 mock 不是系统 | 图运行时永不 mock |
| 2 | **真时间混进虚拟时钟** | 随机 flaky，最难查 | 双时钟域 + lint 禁直接取时间 |
| 3 | **用真模型做回归** | 又贵又慢又不稳，改个 prompt 全红 | CI 只跑 ①②③④，真模型只在 M3 |
| 4 | **用假模型评质量** | 自欺（假模型当然觉得编排合理） | 质量断言只在 M3 采样 |
| 5 | **只测 happy path** | C10/C11 才是真风险 | F1–F9 是必须项，不是加分项 |
| 6 | **case 写成代码** | 无法 diff/回放/统计覆盖 | DSL 数据化 |

---

## 11. 需要你拍板

| # | 问题 | 我的倾向 |
|---|---|---|
| Q-m1 | 虚拟时钟是"全局注入"还是"仅测试装配替换"？ | **仅测试装配替换**（生产代码走 `Clock` 接口，测试注入虚拟实现），不在生产开虚拟时钟 |
| Q-m2 | 世界模型要不要持久化（跨重启）？ | **要**（crash-only 测试必须能重启后校验世界），但只在测试内 |
| Q-m3 | `oracle-patch` 的脚本从哪来？ | **v1 手写**；M3 用真模型跑出来的 patch 序列**录制下来**反哺 |
| Q-m4 | 真机影子要不要做 | **先不做**，M0–M2 的结论若与真机冲突再说 |
| Q-m5 | 断言 DSL 用 YAML 还是 TS 字面量 | **YAML**（可 diff、非程序员也能写），复杂断言允许 TS escape hatch |

---

## 12. 一句话

> **先把"给定 patch，系统是否安全"用纯函数锁死（M0），
> 再把世界、时间、故障一个个装进来（M1-M2），
> 最后才让真模型进来校准编排质量（M3）。**
> 顺序反了，就会陷入"用不稳定手段验证不稳定对象"的死循环。
