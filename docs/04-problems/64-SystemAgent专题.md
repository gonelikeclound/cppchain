# 64 · SystemAgent 专题（开题）

> 2026-11-02 开题。本篇是**专题骨架**：先立编号与结构，把「要研究什么」留给逐轮讨论逐步回填。
> 讨论结论一律回写到本文件对应小节，避免散落在会话里——按项目惯例，最终要有可运行 demo 验证。
>
> **《SystemAgent》语义首轮待定**（§1）。先明确的是：它是一个**研究专题**，不是一次性修复——
> 现有检查 ± 后续按「研究 → 设计 → 落地 → 验收」推进，关联的既有资产先盘在 §3 备用，范围定了再裁剪。

---

## 一、开题结论（✅ 2026-11-03 · 第 1~2 轮已定）

> 第 1 轮给出研究源并指认整理任务、第 2 轮拍板 cppchain 落地形态后，本节省去占位、转正为结论。
> 研究源为 `docs/01-discuss/system-agent/`（01–07 + `_research/01–05`）与 `system-agent-doubao` 设计档。

- **定义（已定）**：SA =「**事件驱动 + 状态机驱动**」的**系统级 Agent**，与宿主 DSH 现有 **ReAct 循环式
  Agent** 对照。形状 = `事件 → 状态跃迁 →（仅在声明的接缝处咨询模型）→ 新状态 → … → 终态`；
  核心差异不是"循环 vs 事件"，而是 **谁站在每次跃迁的关键路径上**——LLM 从"每次跃迁必经"降级为
  "显式 gate 上被咨询的专家"，确定性跃迁交机器推进（=`01`/`02` 一句话模型）。
- **空白原语（调研判断）**：显式迁移表 × 可记账 LLM 咨询 × 咨询等待期进程可死——业界普遍三选二；
  v1 倾向先做 B（状态机）+D（咨询）最小闭环。
- **目标（承接设计主张）**：带记账咨询的 statechart 运行时；SA 只编排不执行、动作只作用于图（I1 可验收）。
- **组织形态（Round 2 定，用户表述）**：**SA 不是单个 agent，而是一组 agent**——构建一个
  **SA 核心 agent** + 几个**不同领域的 agent**，各完成不同的工作。核心 agent 负责分诊/编排/跨域协调，
  领域 agent 各自持有域内能力与状态机；整体落在 cppchain 既有多智能体原语上（agent loop /
  subagent / Send / handoff / 子 workflow）。
- **落地载体（Round 2 定）**：demos 单独目录 + mock 独立 so ——
  - 为 SA 提供**专门的 demo 目录**（拟 `examples/sa_demo/`，按仓库 demo 惯例一个可执行目标 + README）；
  - **所有给 SA 专门 mock 的代码放进独立的 `samock` 共享库**（拟 `tests/plugin_demo/samock.cpp` →
    `libsamock.so`，沿用 `sidefx_notify.so` 的先例：`add_library(... SHARED)` + 外部目录 env
    `WFR_TOOL_LIB_DIRS`/`CPPCHAIN_SAMOCK_DIR` 加载，纯 C descriptor + 函数指针、不链 cppchain）。
- **落点（Round 2 定）**：基于 **cppchain** 落地（非 DSH）——DSH 侧零件
  （`ctx.jobs`/`send`/`schedule`/testkit 四件）由 cppchain 既有的 graph/workflow 引擎、agent 循环、
  多智能体原语、副作用治理（journal/审批/审计）承接，缺口收敛为 **Graph Runtime 的 SA 语义层
  （事件漏斗 / 两层上下文 / 咨询 gate / Reaper 善后）** 与 **samock 测试外围**。

## 二、逐轮讨论记录（倒序，最新在前）

### 第 2 轮（2026-11-03 · 拍板 cppchain 落地形态）

- **落点拍板**：基于 **cppchain** 做（非 DSH 宿主）；研究组 01–08 作语义基线，落点时映射到 cppchain 既有件。
- **组织形态拍板**：SA = **一组 agent**（核心 agent + 领域 agents），复用 cppchain 多智能体原语。
- **载体拍板**：SA 专属 **demo 目录** + **独立 `samock` so**（mock 代码全进 samock，参照 `sidefx_notify.so`
  先例：tests/plugin_demo 源 + `add_library(SHARED)` + env 目录加载）。
- 已产出：§六 落地方案骨架（demo / samock / 领域 agent 初集 / 落点映射），待逐条确认后开工。

### 第 3 轮（2026-11-03 · S1 demo 落地）

- 用户拍板 4 点：领域初集 = **A 电话域 + 消息域**；demo 要 **mock 电话**；摆放
  `examples/sa_demo/` + `tests/plugin_demo/samock.cpp`；虚拟钟 v1 用**工具推进**即可。
- **S1 落地并验证（11ms 全过）**：
  - `tests/plugin_demo/samock.cpp` → `libsamock.so`：世界模型（calls/messages/side_effects 只追加台账）+
    三诚实度（honest/lying/partial）+ 虚拟钟（工具推进、确定性从 0 起步）+ v0 能力（make_call/send_message）
    + world_state（断言唯一入口，永远说真话）+ world_reset（跨用例隔离）；纯 C descriptor、不链 cppchain。
  - `examples/sa_demo/main.cpp` 全离线（脚本桩模型，S1 不发起对话）：
    ① samock 装载走现有 **`tool::load_dynamic_tools`**（wfr 动态工具库同一加载器，fail-closed）；
    ② 世界空跑六场景（A 空基线 / B honest·真做+幂等 / C lying·说谎被抓 / D partial·半写 /
      E 虚拟钟 / F 重置），**断言只查世界不查返回值**；③ SA 组装配（核心 + 电话域 + 消息域三实例）+
      I1 检查（核心 agent 工具集全只读）。
  - `./run-demo.sh sa_demo` **验收结论：S1 全部通过**。
- **实现口径（与 §5.2 提案的两点差异，按实况落档）**：`add_library(samock SHARED ../tests/plugin_demo/samock.cpp)`
  建在 **examples/CMakeLists.txt**（与 `mcp_weather_list` 同先例：demo dlopen 的 so 随 examples 构建目录出，
  与 demo 二进制同目录）；装载不手写 dlopen，用现成 `tool::load_dynamic_tools`；samock 定位顺序 =
  env `CPPCHAIN_SAMOCK_DIR` → 编译期 `SA_SAMOCK_DIR`（examples 构建目录）。
- 下一步：**S2** —— 假电话 FSM（Dialing→Ringing→Connected→Ended|Failed）+ barge_in 进 samock，
  C7 简版组织闭环（核心→电话域→消息域，打电话→改口→转消息），先离线脚本桩、再决定真机。在线验证再收口。

### 第 4 轮（2026-11-03 · S2 落地）

- **S2 全过（10ms）**：假电话 FSM 进 samock + C7 简版组织闭环，24 项断言全绿。
- **samock 新增**：
  - **平台侧电话 FSM**：每次拨号 = 一个 Session，`phase = fold(剧本, dialed_at, now)` **纯派生、无后台线程**
    （可回放、逐字节确定）；剧本 = attempt1 振铃@3s/拒接@10s、attempt2 振铃@3s/接通@8s；
  - **里程碑事件**：dialing / ringing / rejected / answered / ended —— 相位跳变时**原子发一次**（幂等）、
    事件时间取精确时刻（非查询时刻）；`samock_milestones` 按 **seq 游标**读增量（= 观测归约）；
  - **`samock_barge_in`**：控制平面注入用户改口（仅 Talking 态有效——负路径留 F 系列故障注入），
    平台挂断并发 `ended{reason=user_redirected}`。
- **demo 新增 S2 导演编排**（全离线，脚本桩模型**弹 tool_calls**、Agent 循环+ToolNode 真跑工具，非导演代跑）：
  核心 2 次咨询（初始分诊 + 改道编排 ≤3）· 电话域域内拒接→重拨 attempt2→接通（核心不醒）·
  barge_in 直灌 · 消息域 send_message 收尾。**24 项断言**：世界终态（c1 ended / user_redirected /
  redirect_to_messaging）· 里程碑全序恰 7 条（dialing:1,ringing:1,rejected:1,dialing:2,ringing:2,answered:2,
  ended:2）· seq==7 · now_sec==27（零真实等待）· 核心模型咨询==2（拒接/振铃/重拨全程未打扰核心，
  C7「唤醒 7→3」方向兑现）· 电话域 9 轮 / 消息域 2 轮。
- 下一步：**S3** —— SA 语义层校验纯函数（V1–V8 子集）+ 坏 patch/坏模型对抗集（07 §5.2）。

### 第 5 轮（2026-11-03 · S3 M0 化落地）

- **S3 全过（13ms）**：SA 语义层校验**纯函数** + 坏编排对抗集 —— 全部不需要 LLM/世界/时钟（07 §0）。
- **新增 `examples/sa_demo/sa_guard.hpp`**（demo 内、纯 std+nlohmann、零 cppchain 依赖）：
  `cppchain::sa` 命名空间的七个纯函数不变式 —— **I00 未知 agent / I01 域外或幻觉工具 /
  I02 核心只读（I1 静态面）/ I03 分诊目标∈已知域 / I04 里程碑∈词汇表 / I05 预算（≈V4）/
  I06 循环迹象（S4）/ I07 任务断言（≈V6）**；辅助：`canonical_json`（参数指纹）、`usage_from_trace`
  （轨迹自证用量，宿主不口报）。
- **V1–V8 取舍（显式落档）**：**V4→I05 · V6→I07**；I01/I03/I04/I06 = 组编排层新增不变式；
  **V1/V2/V3/V5/V7/V8 因 agent-group 形态无 DAG/waits/pivot → 标注 N/A，不硬凑**（等 Graph Runtime
  形态出现再补）。
- **demo 三形态**：① **单元自检**（oracle 直灌 12 条好/坏样本，坏样本必须被拒：幻觉工具 / 越域 /
  未知 agent / 核心写工具 / 不存在域 / 词汇表外里程碑 / 预算超限×2 / S4 循环 / 缺断言）；
  ② **集成过检**（S2 真实轨迹走完整验证链 —— 白名单=实际部署注册表防漂移；用量账轨迹自证：
  core 咨询 2≤3 · phone 7≤8 · msg 1≤4 · 重试 2≤2；里程碑 7 条全∈词汇表）；
  ③ **运行时对抗**（脚本桩弹幻觉工具 `samock_send_sms` → **ToolNode 既有失败关闭**回喂
  `unknown tool` error、咨询不中断、世界零污染）——**分工：验证器=审、ToolNode=挡**。
- 轨迹来源 = **录迹包装**：域注册表 handler 外包一层 recorder，域 agent 每把真实工具调用落 `trace`
  （验证器消费的是运行时事实，不是导演口报）。
- 下一步：**S4+** —— 事件漏斗 / L1 任务表 / Reaper 善后（按 §5.5 表续）。

### 第 1 轮（2026-11-03）

- 用户指认研究源：`docs/01-discuss/system-agent/`（01–07 + `_research/01–05`）+
  `system-agent-doubao/system-agent-desigin.md`，要求整理三件事：**① 核心架构 ② 测试框架 ③ Mock 外围**。
- 产出整理稿 [`system-agent/08`](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md)；
  §一 定义转正（上节）；§三 盘点改为指向真实研究源。
- **待续（08 §四 开放项）**：落点（DSH vs cppchain 平移）；`02 §10.2` Q1–Q12 与 `07 §11` Q-m1–Q-m5
  待拍板项合表收敛；是否以 **M0（patch 直灌 + 验证器纯函数，2~3 天零基建）** 做第一刀 demo。

### 第 0 轮（开题，2026-11-02）

- 用户提出开专题研究 SystemAgent，要求先把专题开起来、内容逐点讨论。
- 本文件 = 编号 64，problem 线开题骨架；README 已知问题清单 + 文件列表已登记。
- 下一步：第一轮讨论定「SystemAgent 指什么」（即 §一，已由第 1 轮完成）。

## 三、研究源盘点（第 1 轮已定位）

- **权威研究源**（SA 的定义与演进全部在此）：`docs/01-discuss/system-agent/` 01–07 + `_research/01–05`——
  02 架构草案为骨骼，01 业界调研 + 03–07 case 反证/推演为修正链；一手基线 `_research/05`（DSH 源码取证）。
- **规格型并行稿**：`docs/01-discuss/system-agent-doubao/system-agent-desigin.md`——无日期无作者，
  分层（五层）与边语义更清、时间停在"待深化问题"；**以 group 演进稿为准**（08 §0.3 差异对照，不合并只标注）。
- **整理稿**：[`system-agent/08`](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md)
  （核心架构 / 测试框架 / Mock 外围三合一，本专题当前研究主线）。
- **宿主与落点（开放项）**：全部设计以 **DSH（deepseek-harness）** 为宿主（`agent-loop` 是唯一含循环逻辑的
  包、不可 fork，SA 以插件寄居循环外）。若后续落 **cppchain**，需为 DSH 的
  `ctx.jobs` / `send(target×wakeup)` / `schedule` / `agent-loop-testkit` / `llm-mock-server` /
  `llm-replay` / `invariants` 找对应物或平移——这是待讨论的落点问题，不是已定事实。CPPChain 侧既有
  多智能体（`mch`/handoff）、project 自管理等仅作外围血缘，非 SA 本体。

## 四、专题流程约定（沿用项目基线）

1. **问题 + demo 驱动**（见 [discuss 01](../01-discuss/01-项目基调与构建原则.md)/README）：每个论点
   要有可运行验证，不写空头方案；真机模型按项目惯例（DeepSeek-flash 够用时不上大模型）。
2. 推进节奏：研究/盘点（本篇）→ 设计（arch）→ 落地（detail / problems 验收）→ 全仓测试 + 真机对照。
3. 数字与编号纪律：全仓串行测试全绿后才写「验收」结论；文档与报告数字一致性是硬要求。

## 五、落地方案骨架（cppchain · 提案，待逐条确认）

### 5.1 SA 的组成：核心 agent + 领域 agents（一组 agent）

```
                        ┌──────────────────────────────┐
   事件 / 用户 query ──► │   SA 核心 agent（编排者）        │
                        │  分诊 → 选域 → 编排拓扑 → fallback │
                        └───────┬──────────┬──────────┘
                     handoff / Send / 子 workflow / ask_human
              ┌───────────┴───┐         ┌───┴───────────┐
       ┌──────▼──────┐  ┌─────▼──────┐  ┌▼──────────────┐
       │ 领域 agent A │  │ 领域 agent B│  │ 领域 agent …   │
       │ 域内能力+FSM  │  │ 域内能力+FSM │  │               │
       └──────┬──────┘  └─────┬──────┘  └┬──────────────┘
              │  工具均指向 samock 世界   │
              └─────────► [libsamock.so 世界 / 假能力 / 事件源]
```

- **核心 agent**：不做域内执行，只决定"谁来做、怎么做、做不了怎么办"（对应研究稿
  「SA 只编排不执行」「分诊=编排的退化结论」）；用 cppchain 的 agent loop / handoff / 子 workflow 落地。
- **领域 agent**：各持一个领域的能力与（可选）状态机，执行域内工作；对应研究稿三类节点里的
  **AgentLoop 节点**（封闭 ReAct 循环，只上报终态与里程碑）。
- **事件入口**：query / 模拟事件流统一进核心 agent；C6 式漏斗若第一版不建，先用目录级最小兜底
  （事件直接进核心 agent，数量少时成本可控），漏斗留待第二版。

### 5.2 目录与构建（沿用仓库先例）

```
examples/sa_demo/                    # SA 专属 demo 目录（一个可执行目标）
  README.md                          # 索引行 + 运行方式
  main.cpp                           # 组装核心 agent + 领域 agents + samock 装载 + S2 导演 + S3 验证
  sa_guard.hpp                       # S3（M0）：SA 语义层校验纯函数（I00–I07，零 cppchain 依赖）
  scenarios/                         # 场景数据（C7 简版 / 对抗坏 patch…），先内联后抽数据
  # 登记到 examples/CMakeLists.txt 的 EXAMPLE_TARGETS（sa_demo → SA_EXAMPLE_TARGETS，链 cppchain_agent+tool）

tests/plugin_demo/samock.cpp         # 所有 SA 专属 mock 代码 → libsamock.so
examples/CMakeLists.txt              # add_library(samock SHARED ../tests/plugin_demo/samock.cpp)
                                     # 先例同 mcp_weather_list（demo dlopen 的 so 随 examples 构建出、
                                     #   与 demo 同目录）；纯 C descriptor（tool_plugin.hpp）、不链 cppchain；
                                     # add_dependencies(sa_demo samock)；
                                     # compile-def SA_SAMOCK_DIR = examples 构建目录
```

- demo 运行：`./run-demo.sh sa_demo`（装载走现有 **`tool::load_dynamic_tools`**，即 wfr 动态工具库同一加载器；
  samock 定位顺序 = env `CPPCHAIN_SAMOCK_DIR` → 编译期 `SA_SAMOCK_DIR`）。
- **隔离纪律**（07 R5）：CI/常规测试路径**不得触碰真模型**；demo 里 LLM 可走 .env 真机
  （延续 chat_live/phantasma 惯例）或走离线对博弈（scripted/adversarial），二者物理分开。

### 5.3 samock.so 第一版装什么（全对 mg 拟件，N 编号沿用研究稿）

| 件 | 研究稿 | samock v1 落地 | 状态 |
|---|---|---|---|
| 世界模型 | N2 | 内存 `World`（calls/messages/side_effects 只追加台账 + seq 单调序号）+ 三诚实度
  （honest/lying/partial）；`world_state` 序列化整个世界（**断言唯一入口，永远说真话**）；重置含钟/诚实度 | ✅ **S1 已装**（`samock_world_state`/`samock_world_reset`；断言逻辑在 demo 侧 C++，见 07「查世界不查返回值」） |
| 假电话 FSM | N3 | 每次拨号=Session，`phase=fold(剧本,dialed_at,now)` 纯派生；剧本=attempt1 拒接/attempt2 接通；
  里程碑（dialing/ringing/rejected/answered/ended）seq 游标增量读；`samock_barge_in` 控制平面（仅 Talking 有效） | ✅ **S2 已装**（`samock_make_call`+`samock_milestones`+`samock_barge_in`；消息侧仍 v0） |
| 事件源 | — | `event_storm`（C6 洪峰注入）+ 源侧聚合（G-l 简化版） | ⏳ 待 S2+ |
| 虚拟时钟 | N1 | **工具推进**的模拟钟（`samock_clock_forward`，确定性从 0 起步、累加式）；全套 quiescence
  调度器留待 M2 需要时再上 | ✅ **S1 已装** |
| 场景 DSL | N4 | S1 场景内联在 demo（C++ 数据直灌工具），抽 YAML 作为 M1 升级项 | 🔶 S1 内联（六场景脚本），YAML 化待后续 |

> 领域 agent 不是 mock，是真实 demo 代码（在 `examples/sa_demo/` 内）；**mock 的一切都进 samock.so**。

### 5.4 DSH → cppchain 落点映射（待落地时逐个实核）

| DSH 现成件（研究稿 _research/05） | cppchain 对应 | 处置 |
|---|---|---|
| `ctx.jobs`（句柄+增量+取消） | 待盘点：tool/workflow 长任务形态（command_run / 并行波 Send） | 实核后补或列为缺口 |
| `send(target×wakeup)`（事件→对话准入） | 待盘点：cppchain 事件/唤醒等价物 | 可能需 SA 宿主循环承接 |
| `schedule`（持久化定时+冷恢复） | wfr cron（触发接入，problems/56）浅近似 | 持久化续体=研究 G2，列缺口 |
| `agent-loop-testkit` / `llm-mock-server` / `llm-replay` / `invariants` | cppchain 测试侧离线 mock model / 断言基建 | 实核后确认可平替项 |
| agent 循环（不可 fork） | cppchain `agent/agent.hpp` 即库里循环，天然可组合 | 直接用 |

**真正要新建的（cppchain 版结论，对应研究稿"只新建 Graph Runtime"）**：SA 语义层
——事件漏斗（观测归约）、L1 任务表 + L2 瞬时装配、咨询 gate（LLM 只出现声明的接缝）、
4 级升级 + Reaper 善后、patch 记账与验证映射到 workflow IR 校验。

### 5.5 里程碑进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **S1** | `examples/sa_demo` 骨架：核心 agent + 电话域/消息域 agents + samock 装载 + 世界空跑六场景 + 组装配/I1 | ✅ **2026-11-03 全过**（`run-demo.sh sa_demo`，11ms） |
| **S2** | samock **假电话 FSM**（Session 剧本 fold + 里程碑 seq 游标 + barge_in 控制平面）→ demo 跑通 **C7 简版**（打电话→改口→转消息，离线脚本桩组织闭环，24 断言） | ✅ **2026-11-03 全过**（10ms） |
| **S3** | M0 化：SA 语义层校验纯函数（V1–V8 子集 → I00–I07）+ 坏编排对抗集（07 §5.2）+ 运行时挡幻觉工具 | ✅ **2026-11-03 全过**（13ms） |
| S4+ | 事件漏斗 / L1 任务表 / Reaper 善后 / 落地验收（C1–C12 抽角 C2/C3/C4/C6/C7/C8） | ⏳ 下一步 |

**落地前已拍板**：领域初集 = **电话域 + 消息域**（A 组）；demo 需要 **mock 电话**；
摆放 = `examples/sa_demo/` + `tests/plugin_demo/samock.cpp`；虚拟钟 v1 = **工具推进**的模拟钟（够用）。

## 六、关联

- 研究主线：[`docs/01-discuss/system-agent/`](../01-discuss/system-agent/README.md)（01–07 + `_research`）+
  整理稿 [08](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md) +
  doubao [设计档](../01-discuss/system-agent-doubao/system-agent-desigin.md)。
- 编号承接：problems 最大 63（[长上下文蜀山案例停置收口记录](63-长上下文蜀山案例停置收口记录.md)）；
  本篇=64，后续子篇顺延。
- 相邻面：`高可信/`（做得对不对）+ `主动智能/`（什么时候开始）= SA 相邻（SA 管"开始之后以什么形态运行"）。
- 内存关联：`cppchain-rsi-plan`（Agent 自进化）、`cppchain-personal-agent-plan`。