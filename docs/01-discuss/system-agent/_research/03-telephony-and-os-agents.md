# 03 · 电话/语音状态机与 OS Agent 调研笔记

> 目的：为「事件驱动 + 状态机驱动」的系统级 Agent（SystemAgent）收集一手证据。
> 方法：优先抓一手文档（RFC、Android/Apple/Google 官方文档、Twilio/LiveKit/Pipecat/Vapi 官方站），
> 论文以 arXiv abs 页为准、ID 逐一核对。未核实到的一律写「未找到」。

**两句话结论**

1. **电话侧**：没有任何一家把「打一通电话」建模成"一次阻塞调用等返回"。全部是 **协议内部 FSM + 对业务暴露事件回调 + 业务侧外部持久化状态** 的三段式。
2. **OS Agent 侧**：主流仍是 **ReAct 式"截图→想→点"线性循环**；用状态机/程序结构做控制层的工作 2025 年才出现（Agent-SAMA、SPlanner、AgentProg）且增益可测；但**未找到**把"OS 事件流 + 进程被杀后续体恢复"做成系统的工作。

---

# A 部分：电话/语音会话——长生命周期状态机怎么落地

## A1. SIP / RFC 3261：事务层与会话层两套正交状态机

**（1）事务层只管一条请求/响应的可靠性**（§17，Figure 5 / Figure 7 / §17.1.2 / §17.2.2）：

```
INVITE client:  Calling --1xx--> Proceeding --2xx--> Terminated
                   |                  |
                   +--300-699--> Completed --Timer D--> Terminated
INVITE server:  Proceeding --1xx/2xx/300-699--> Completed --ACK--> Confirmed --Timer I--> Terminated
non-INVITE:     Trying --> Proceeding --> Completed --> Terminated
```

两个关键细节直接反驳"阻塞式调用"：`Calling` 状态下 `Timer A`（重传）/`Timer B`（超时）在跑，超时后**不是抛异常，而是 "inform TU"**（向上一层上报事件）；事务层对上一层（Transaction User）只上报事件，本身不做业务决策。
<https://www.rfc-editor.org/rfc/rfc3261.html>

**（2）会话层（dialog）管一整通电话**（§12、§13.2.2.3）。RFC 区分 **early dialog**（1xx 建立）与 **confirmed dialog**（2xx 确认），且**一个 INVITE 可因 forking 产生多个 early dialog**，只有被 2xx 确认者成为正式 dialog，非 2xx 终态应答时"All early dialogs are considered terminated"。

工程含义：**业务方看到的"通话"其实是 (Call-ID, local tag, remote tag) 三元组的一个集合**，必须由上层状态机收敛成"用户语义上的一个通话"。传输层给的是事件流，不是对象。

**（3）业务语义上的"通话"是第三层状态机**（下图为按 §13/§15 语义归纳，非 RFC 原图）：

```
[Idle] ─拨号/INVITE→ [Calling] ─1xx(180 Ringing)→ [Ringing] ─2xx+ACK→ [Active]
                        │                            │                  │
                        │ 3xx-6xx / Timer B          │ 486 / 487        │ BYE
                        ▼                            ▼                  ▼
                     [Failed] ───────────────→ [Terminated] ←──────────┘
                                                  ▲
                                    从 Calling/Ringing 的 CANCEL ─┘

[Active] ⇄ [Holding]：re-INVITE（sendonly / sendrecv）切换，不离开主路径
```

三层各自独立演进：事务层管重传与超时、dialog 层管身份与路由、业务层管"这通电话对我们意味着什么"。
**SystemAgent 要复刻的正是第三层**——前两层由 OS/平台提供。

## A2. Android Telecom：`Connection`（服务侧）与 `Call`（业务侧）两套并行枚举

| 侧 | 类 | 状态枚举（文档原文，含值） |
|---|---|---|
| 电话实现方（ConnectionService） | `Connection` | `STATE_INITIALIZING=0` `STATE_NEW=1` `STATE_RINGING=2` `STATE_DIALING=3` `STATE_ACTIVE=4` `STATE_HOLDING=5` `STATE_DISCONNECTED=6`，另有 `STATE_AUDIO_PROCESSING` `STATE_SIMULATED_RINGING` `STATE_PULLING_CALL` |
| 通话业务/UI（InCallService） | `Call` | `STATE_NEW=0` `STATE_DIALING=1` `STATE_RINGING=2` `STATE_HOLDING=3` `STATE_ACTIVE=4` `STATE_SELECT_PHONE_ACCOUNT=8` `STATE_CONNECTING=9`，另有 `STATE_DISCONNECTING` `STATE_DISCONNECTED` `STATE_AUDIO_PROCESSING` `STATE_SIMULATED_RINGING` `STATE_PULLING_CALL` |

API 形态的三个要点：

- 观察状态**只能靠回调**：`Call.registerCallback(Call.Callback)`，回调含 `onStateChanged(Call,int)`、`onDetailsChanged(Call,Call.Details)`、`onCallDestroyed(Call)`。
- 直接点查的 `Call.getState()` **在 API 31 已 deprecated**，官方把状态搬进 `Call.Details.getState()` —— **状态是随事件送达的快照，不是随时可读的全局变量**。
- 动作是异步指令：`hold()` / `unhold()` / `answer(int)` / `reject(int)` / `disconnect()` / `playDtmfTone(char)` / `postDialContinue(boolean)` —— 发命令，等下一次 `onStateChanged` 告诉结果。

> <https://developer.android.com/reference/android/telecom/Connection> · <https://developer.android.com/reference/android/telecom/Call>

**这就是 SystemAgent 想要的形态的现成范本**：OS 提供状态枚举 + 回调分发 + 命令式动作，业务方自己维护"当前该显示/该做什么"。

## A3. Twilio Programmable Voice：把状态机彻底变成 webhook 事件

**(1) TwiML 文档 ≈ 状态，`action` URL ≈ 转移。** 官方原文："Twilio executes **just one TwiML document** to the caller at a time, but many TwiML documents can be linked together"。这正是"状态机 + 转移表"的等价物。<https://www.twilio.com/docs/voice/twiml>

**(2) Call 状态枚举**：`queued` → `ringing` → `in-progress` → `completed`，分支终态 `canceled` / `busy` / `no-answer` / `failed`。

**(3) `StatusCallbackEvent` = 状态变化的异步 HTTP POST**：「Twilio will make an **asynchronous request** to the `StatusCallback` URL」；事件 `initiated`（出队开始拨号）/`ringing`（开始振铃）/`answered`（被叫接听，会顺带发 `in-progress`）/`completed`（无论终止原因；未指定时默认只发 `completed`）。回调携带 `CallStatus`、`Duration`、`CallDuration`。→ **事件语义（发生了什么）与结果语义（最终状态）分离**。<https://www.twilio.com/docs/voice/api/call-resource>

**(4) `<Dial>` 的 `action`：子通话也是状态机节点。** 触发回调时带 `DialCallStatus`、`DialCallSid`、`DialCallDuration`、`DialBridged`、`RecordingUrl`；结束条件含被叫未接、忙音、主叫挂断等。<https://www.twilio.com/docs/voice/twiml/dial>

**(5) Media Streams：接通后是双向事件流，不是 RPC。** WebSocket 消息类型 `connected`（带 protocol/version）→ `start`（带 `streamSid`/`callSid`/`tracks`/`mediaFormat`/`customParameters`）→ `media`（mulaw 8k 音频帧）→ `dtmf` → `mark` → `stop`，均带 `sequenceNumber` 保序。<https://www.twilio.com/docs/voice/media-streams/websocket-messages>

**业务方拿到的**：一个 `callSid` 主键 + 一串异步 POST + 一个 WebSocket 帧流。状态存在**你的数据库里**，不存在任何调用栈里。

## A4. LiveKit Agents / Pipecat：会话内部同样是状态机 + 打断语义

**LiveKit**

- **job 生命周期**：agent server 接受 dispatch 后**起新进程**跑 agent，"Each job runs in a separate process... If a session instance crashes, it doesn't affect other agents"；job 终止条件 = **房间内所有标准参与者与 SIP 参与者都离开**或显式关闭。→ 会话"结束"是**外部事件**，不是函数返回。<https://docs.livekit.io/agents/server/job/>
- **turn detection**：turn detector 模型（默认）/ realtime 模型内置 / 纯 VAD / STT endpointing / **manual**（`session.interrupt()`、`session.commit_user_turn()`）。→ 轮次边界是**可插拔的显式判定器**。
- **打断**：`InterruptionOptions`，`mode="adaptive"` 用模型区分"真打断"与"附和(backchannel)"；**false interruption**（VAD 触发但转写为空）→ `false_interruption_timeout` 后发 `agent_false_interruption` 事件，`resume_false_interruption=True` 时**从中断处继续说话**。
- **不可打断语音**：`allow_interruptions=False` 的句柄 `interrupt()` 会报错、需 `force`；队列遇首个不可打断项就停在那儿继续播。→ **每个 utterance 自带打断策略 = 状态机上的 guard 条件**。
- **workflow 构造**：Agent（长生命周期持有控制权）/ Task（跑完返回 typed result、不持久）/ **TaskGroup（有序多步，支持回退到已完成的早期步骤）** / Handoff（控制权完全转移，可传/摘要/清空上下文）。

> <https://docs.livekit.io/agents/build/turns/> · <https://docs.livekit.io/agents/logic/workflows.md>

**Pipecat：打断 = 一条广播控制帧 + 取消 in-flight 工作**

`InterruptionFrame` 是 SystemFrame，**同时向上游与下游广播**，每个 processor 立即（不等队列）取消自己的任务、丢弃排队中的 `DataFrame`/`ControlFrame`；`interruptible=False` 的帧（`FunctionCallResultFrame`、`EndFrame`）被保留继续处理；in-flight LLM completion 中途取消，`cancel_on_interruption=True` 的函数调用被取消并发出 `FunctionCallCancelFrame`；TTS 清缓冲；transport 丢弃"已生成未播放"的音频。上下文**只提交真正说出口的部分**（`TTSTextFrame` 随播放同步下发）。<https://docs.pipecat.ai/pipecat/fundamentals/interruptions>

> 这是"**中断即状态转移 + 副作用可撤销 + 上下文与物理世界对齐**"的一手实现，可直接借到 SystemAgent 的工具调用语义里。

## A5. Vapi 等平台：事件全是 webhook POST

所有消息都是 `POST`，体为 `{"message": {"type": "...", "call": {...}}}`。**绝大多数是通知型**（"do not require a response"）；只有 4 类需同步应答：`assistant-request`、`tool-calls`、`transfer-destination-request`、`knowledge-base-request`。另有 `call.ringing`、`call.ending`（可挂在 phone number 的 hooks）、`call.endpointing.request`、`voice-request`（发往专用 server），以及 `listenUrl` WebSocket 实时事件流。<https://docs.vapi.ai/server-url/events> · <https://docs.vapi.ai/server-url>

（注：`https://docs.vapi.ai/calls/call-events` 已 404，职责由 `server-url/events` 承担；Bland 的可引用状态事件页本轮**未找到**。）

## A6. 结论：业界如何把"通话"这种状态机交给业务逻辑？

四个平台互为印证，答案高度一致：

1. **协议/框架内部是严格 FSM + 定时器**（RFC 事务、Android `Connection`、LiveKit job）。
2. **对业务暴露的是事件，不是返回值**（SIP "inform TU"、`onStateChanged`、`StatusCallbackEvent`、Vapi server message、`agent_false_interruption`）。
3. **业务状态存在业务侧外部存储**，以 `callSid`/`call_id`/job id 为聚合根。唯一出现的"阻塞 API"（`calls.create()`）只表示"受理成功"，不表示"通话结束"。
4. **回调至少一次**：事件带 sequence/幂等键（Twilio `sequenceNumber`）、终态事件兜底（`completed` 总会发）、丢失时用 API 反查（`Fetch a Call`）。
5. **中断/取消是头等公民**，且需显式"撤销 / 续播 / 回退"语义（`resume_false_interruption`、`FunctionCallCancelFrame`、TaskGroup 回退）。

> 一句话：**"拨号→振铃→接通→挂断"从来不是一次函数调用，而是一个有身份、可中断、事件可重放的状态机。**

---

# B 部分：操作系统 / 移动端 Agent 的研究现状与长流程局限

## B1. 主流执行范式：几乎无一例外是 ReAct 式线性循环

| 工作 | arXiv | 执行范式 |
|---|---|---|
| AndroidWorld | [2405.14573](https://arxiv.org/abs/2405.14573) | 基准 116 任务 / 20 app；基线 agent = 每步"截图→LLM→动作"循环 |
| OSWorld | [2404.07972](https://arxiv.org/abs/2404.07972) | 基准 369 任务，Ubuntu/Windows/macOS 真实环境 |
| Mobile-Agent (v1) | [2401.16158](https://arxiv.org/abs/2401.16158) | 视觉感知工具定位元素 → 单 agent 逐步规划执行 |
| Mobile-Agent-v2 | [2406.01014](https://arxiv.org/abs/2406.01014) | 单 agent 拆成 planning / decision / reflection 三 agent + 记忆单元 |
| Mobile-Agent-v3（GUI-Owl） | [2508.15144](https://arxiv.org/abs/2508.15144) | 端到端基础模型 + 框架；AndroidWorld 73.3 / OSWorld 37.7 |
| AppAgent | [2312.13771](https://arxiv.org/abs/2312.13771) | 简化动作空间（tap/swipe）；探索或看演示 → 生成知识库 |
| AutoDroid | [2308.15272](https://arxiv.org/abs/2308.15272) | LLM + 动态分析得功能感知 UI 表示 + 探索式记忆注入 |
| Mobile-Agent-E | [2501.11733](https://arxiv.org/abs/2501.11733) | **分层**：Manager 拆 subgoal + Perceptor/Operator/Action Reflector/Notetaker |

前七个都是"单步决策 + 线性历史"的循环体（ReAct 或其变体）；Mobile-Agent-E 是首个明确做**高层规划/低层执行分离**的移动端框架。但**没有一个**把"OS 事件"作为循环驱动源 —— 驱动源始终是"上一步动作完成 + 重新截图"。

## B2. 论文自己点名的长流程失败模式（原文措辞）

1. **迷失/错误累积**：SPlanner 指出 agent "lack a deep understanding of how to effectively use the target applications, which can cause them to become **'lost'** during task execution"（[2505.14141](https://arxiv.org/abs/2505.14141)）。
2. **反应式、无导航流表示 → 无法发现异常、无法恢复**：Agent-SAMA 原文 "existing agents remain **fundamentally reactive**. They reason over the current UI screen but **lack a structured representation of the app navigation flow**, limiting GUI agents' ability to understand execution context, **detect unexpected execution results, and recover from errors**"（[2505.23596](https://arxiv.org/abs/2505.23596)）。
3. **过度执行 / 执行不足 / 注意力漂移**：UI-NEXUS 总结出 under-execution、over-execution、attention drift 三类代表性失败模式，并指出现有 agent 无法兼顾性能与效率（[2506.08972](https://arxiv.org/abs/2506.08972)）。
4. **Token/上下文爆炸**：Mobile-Agent-v2 归因于 "the overly long token sequences and the interleaved text-image data format"；AgentProg 指出 "reliance on ever-expanding interaction history incurs substantial context overhead"，且现有压缩方法 "fail to preserve vital semantic information"（[2512.10371](https://arxiv.org/abs/2512.10371)）。
5. **错误恢复难 + 冷启动**：MobileUse 把 "long-horizon task execution, difficulty in error recovery, and the cold-start problem in unfamiliar environments" 列为主要挑战（[2507.16853](https://arxiv.org/abs/2507.16853)）。
6. **绝对成功率仍很低**：OSWorld 人类 72.36% vs 最佳模型 **12.24%**；AndroidWorld 论文自报最佳基线 **30.6%**（[2404.07972](https://arxiv.org/abs/2404.07972)、[2405.14573](https://arxiv.org/abs/2405.14573)）。
7. **对任务变体极脆弱**：AndroidWorld 的 robustness analysis 显示 "task variations can significantly affect agent performance"，说明学到的是轨迹拟合而非流程。

> **未找到**专门量化"系统弹窗/异步事件（通知、权限框、来电、后台被杀）导致失败"的独立论文。

## B3. 确实引入状态机 / 分层确定性执行的工作（找到了，且增益可测）

1. **Agent-SAMA（最直接对标）** —— 把 app 执行建模为 **FSM：UI 屏幕 = 状态、用户动作 = 转移**；四个 agent 实时构建并消费 FSM 来指导规划、执行校验与恢复。Mobile-Eval-E **84.0% 成功率 / 71.9% 恢复率**；SPA-Bench 80.0% / 66.7%；AndroidWorld 63.7%；较此前方法成功率 **+12%**、恢复成功率 **+13.8%**。作者结语："structured state modeling enhances robustness and **can serve as a lightweight, model-agnostic memory layer**"（[2505.23596](https://arxiv.org/abs/2505.23596)）。→ 状态机作为"控制器 + 记忆层"最有力的一手证据。
2. **SPlanner（EFSM 规划模块）** —— 用 **扩展有限状态机建模 app 的控制逻辑与配置**，把指令分解为 EFSM 主功能序列、**遍历 EFSM 生成执行路径**，再由 LLM 精炼成自然语言计划。AndroidWorld + Qwen2.5-VL-72B = 63.8%，**比无规划 +28.8 个百分点**（[2505.14141](https://arxiv.org/abs/2505.14141)）。→ **planner 确定性 + executor 模型化**，正是 SystemAgent 想要的组合。
3. **AgentProg（程序化上下文 + Belief MDP 信念状态）** —— 把交互历史重构为"带变量与控制流的程序"，用程序结构决定"哪些信息该留/该丢"，并引入 global belief state 处理部分可观测与意外环境变化。长流程上基线出现 "catastrophic degradation" 而它保持稳定（[2512.10371](https://arxiv.org/abs/2512.10371)）。
4. **Mobile-Agent-E（分层 + 技能固化）** —— 自演化模块维护长期记忆：`Tips`（经验规则）与 `Shortcuts`（**可复用的原子操作可执行序列**）；Mobile-Eval-E 上 **+22%** 绝对提升（[2501.11733](https://arxiv.org/abs/2501.11733)）。→ Shortcuts = 把探索结果固化为确定性宏。
5. **Voyager（技能库固化原型）** —— "ever-growing skill library of executable code"，技能 temporally extended、interpretable、compositional，缓解灾难性遗忘（[2305.16291](https://arxiv.org/abs/2305.16291)）。
6. **AutoManual（SOP/规则手册化）** —— Planner 按当前规则编码计划，Builder 在线更新结构化规则，Formulator 编译成人可读手册；ALFWorld **97.4%（GPT-4-turbo）/ 86.2%（GPT-3.5-turbo）**（[2405.16247](https://arxiv.org/abs/2405.16247)）。→ 小模型 + 手册 > 大模型裸跑。
7. **MobileUse（分层反思）** —— 在"单动作/子任务/整体任务"多时间尺度自监控与恢复，reflection-on-demand 控开销（[2507.16853](https://arxiv.org/abs/2507.16853)）。
8. **AGENT-NEXUS（长流程动态分解调度）** —— 把长流程任务动态分解为一串自包含原子子任务，对现有 agent 提升 **24%~40%**（[2506.08972](https://arxiv.org/abs/2506.08972)）。

**缺口（诚实标注）**

- 上述工作都是**"计划层/记忆层用 FSM"**，**没有一个**把"进程被杀 → 等待中的续体恢复"当核心问题。
- Agent-SAMA 的 FSM 是**从 UI 观察实时推断**的，不持久化、不可重放 —— 与"可恢复的状态机"仍是两回事。
- **未找到**把"OS 级事件（通知/来电/权限框/广播/后台限制）"作为一等状态机驱动源的 agent 论文。
- **未找到**移动端 agent 领域的 durable execution / checkpointer 工作。

## B4. Android / Apple 平台机制：是否天然要求"异步 + 事件回调"？——是，且在签名层面强制

**(1) FGS 与后台执行限制。** 官方原文："Apps that target Android 12 (API level 31) or higher **can't start foreground services while the app is running in the background**, except for a few special cases"，否则抛 `ForegroundServiceStartNotAllowedException`。豁免情形几乎全是**"收到某个事件"**：从用户可见状态迁移、高优先级 FCM、用户点了通知/小组件/气泡、`ACTION_BOOT_COMPLETED` 广播、精确闹钟、地理围栏、NFC 交易、时区/时间/语言变更……

更狠一条：**需要 while-in-use 权限（麦克风/相机/位置/身体传感器）的 FGS 在后台根本不能创建**，且后台时 `checkSelfPermission()` 仍返回 `PERMISSION_GRANTED`（"while in use"语义），故 Android 14+ 直接抛 `SecurityException`。<https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start>

> **含义**：长流程的"等待"不可能靠"进程里挂一个协程等着"——系统会在后台时堵死这条路。**等待必须外化为可持久化状态 + 由事件重新唤醒。**

**(2) `AppFunctions`（Android 16+）：官方的 app 能力暴露协议。** 定位是 **"the mobile equivalent of tools within the Model Context Protocol (MCP)"**，app 变成 on-device MCP server，向 OS 内置 registry 注册函数。

- 形态：`@AppFunction suspend fun createTask(...): Task` —— **`suspend`**；失败用预定义异常表达（`AppFunctionInvalidArgumentException`、`AppFunctionElementNotFoundException`），"Developers can use predefined exceptions to let the agent know why it failed"。
- 发现：Jetpack 注解处理器生成 XML schema，由 **OS 索引**；调用方（须持 `EXECUTE_APP_FUNCTIONS`）经 `AppFunctionManager` 发现与执行，可先用 `isAppFunctionEnabled(pkg, fnId)` 探测。
- app 级元数据可声明 "operational patterns and constraints" 指导跨工具编排。状态：实验性，Android 16+，与 Gemini 的集成于 2026-05 处私有预览。<https://developer.android.com/ai/appfunctions>

> **含义**：官方给 agent 的接口是"**注册能力 + 异步调用 + 结构化异常 + 用前探测**"，而不是"给我一个能跑很久的阻塞会话"。`isAppFunctionEnabled` 这步本身就承认了"能力存在 ≠ 此刻可用"。

**(3) Apple `App Intents`。** "Make content and actions discoverable by Apple Intelligence and support system experiences like Siri, Spotlight, Shortcuts, and widgets" —— 用结构化方式表达 app 的 actions 与 data，供系统智能与 Siri/Shortcuts 发现与调用。<https://developer.apple.com/documentation/appintents>（`@Parameter` 解析、`AppEntity` 查询、`perform()` 异步语义在 JS 渲染子页，本轮未逐页核实。）

## B5. Google 对 agent 与系统能力集成的形态：Gemini Live

- **协议**：Stateful WebSocket（WSS）；输入 raw PCM 16kHz / 图像 ≤1FPS / 文本，输出 PCM 24kHz。<https://ai.google.dev/gemini-api/docs/live>
- **硬限制**：音频 context 约 2 分钟、**连接寿命约 10 分钟**，"When the connection terminates, the session terminates as well"。
- **显式恢复机制**：`sessionResumption`（服务端下发 `SessionResumptionUpdate`，token 作为下一次连接的 `SessionResumptionConfig.handle`，**最后一次会话终止后 2 小时内有效**）；断连前先发 `GoAway` 让你"take further actions"；`contextWindowCompression`（sliding window）把会话延长到 unlimited。<https://ai.google.dev/gemini-api/docs/live-session>
- **非阻塞工具调用**：函数定义可标 `behavior: "NON_BLOCKING"` → "will run asynchronously"，不阻塞与模型交互；响应到达时用 `scheduling` 指定 `"INTERRUPT"` / `"WHEN_IDLE"` / `"SILENT"`。<https://ai.google.dev/gemini-api/docs/live-tools>

> 三件事（连接会被断、断前先给 GoAway、异步结果用 scheduling 决定何时呈现）恰好就是"事件 + 状态 + 续体恢复"在工业协议里的最小完备集。`WHEN_IDLE`/`SILENT` 更是直接承认"**结果到达时机 ≠ 请求时机**"。

## B6. B 部分小结

- 学术侧：**范式仍以线性 ReAct 为主**，但 2025 年起出现**用 FSM/程序结构做控制层**的明确转向，且增益可测（+28.8pp、恢复率 71.9%）。
- 平台侧：**官方 agent 接口本身就是异步 + 事件 + 能力注册**（AppFunctions 的 `suspend`、Gemini Live 的 GoAway/resumption/NON_BLOCKING、Android FGS 的后台禁令）。
- 缺口：**没有任何工作把"进程被杀 + 等待中的续体恢复"当核心问题**，也没有把 OS 异步事件当一等驱动源。

---

## 对 SystemAgent 的证据支持

1. **电信协议层本身就是分层状态机，且只向上层"上报事件"。** RFC 3261 的 INVITE client/server transaction 各有 `Calling/Trying→Proceeding→Completed→Confirmed→Terminated`，超时与错误都是 "inform TU"。<https://www.rfc-editor.org/rfc/rfc3261.html>
2. **移动 OS 已把该模式做成公共 API 契约。** Android `Connection`/`Call` 用枚举 + `registerCallback` 暴露通话状态，`getState()` 已被 deprecate 迁到回调快照 `Call.Details.getState()` —— 状态随事件送达。<https://developer.android.com/reference/android/telecom/Call>
3. **规模化语音业务平台，业务逻辑 = 无状态 webhook handler + 外部状态存储。** Twilio `StatusCallbackEvent` 是**异步 HTTP 请求**；`initiated/ringing/answered/completed` 与终态 `busy/no-answer/failed/canceled` 分离；`<Dial action>` 用 `DialCallStatus` 回传子通话结果。<https://www.twilio.com/docs/voice/api/call-resource> · <https://www.twilio.com/docs/voice/twiml/dial>
4. **"通话"的持久身份是 `callSid`/`call_id`，而非栈帧。** Twilio Media Streams `start` 显式带 `callSid`/`streamSid`/`sequenceNumber`；Android 侧用 `Call` 对象 + `DisconnectCause` 表达终态。→ SystemAgent 需要 **run id + 事件日志**。<https://www.twilio.com/docs/voice/media-streams/websocket-messages>
5. **中断可恢复，而非"失败重来"。** LiveKit `resume_false_interruption` + `false_interruption_timeout` 在误打断后**从中断处继续**；Pipecat `InterruptionFrame` 取消 in-flight LLM/TTS 但保留 `interruptible=False` 的结果帧，上下文只提交"真正说出口的部分"。<https://docs.livekit.io/agents/build/turns/> · <https://docs.pipecat.ai/pipecat/fundamentals/interruptions>
6. **长连接寿命有限，恢复必须显式建模。** Gemini Live：连接约 10 分钟、音频 context 约 2 分钟、断连即会话终止；为此提供 `sessionResumption`（token 有效 2 小时）、断前 `GoAway`、`contextWindowCompression`。<https://ai.google.dev/gemini-api/docs/live-session>
7. **"结果到达时机 ≠ 请求时机"是协议级公认问题。** Gemini Live 的 `behavior: NON_BLOCKING` + 响应 `scheduling: INTERRUPT | WHEN_IDLE | SILENT`，即异步结果何时注入必须由调用方向声明。<https://ai.google.dev/gemini-api/docs/live-tools>
8. **移动 OS 在制度层面禁止"后台挂着一个等着"。** Android 12+ 后台不能起 FGS；需 while-in-use 权限的 FGS 在后台不能创建（`SecurityException`）；豁免几乎都是"收到某个事件"。→ 长等待必须外化为持久状态 + 事件唤醒。<https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start>
9. **（学术侧）显式 FSM 作为控制层已拿到可测量增益。** Agent-SAMA 恢复率 71.9%、AndroidWorld 63.7%；SPlanner 用 EFSM 生成执行路径，AndroidWorld +28.8pp。<https://arxiv.org/abs/2505.23596> · <https://arxiv.org/abs/2505.14141>
10. **（学术侧）"反应式线性循环"被论文原文定性为根本缺陷。** Agent-SAMA：agent "remain **fundamentally reactive**... lack a structured representation of the app navigation flow, limiting... detect unexpected execution results, and recover from errors"。<https://arxiv.org/abs/2505.23596>

## 反面证据

1. **平台厂商自己劝你先别上结构。** LiveKit 原文："Each pattern you layer on top of a single agent adds **complexity, latency, or context management overhead**. Split workflows only when you encounter a concrete limitation"（触发条件：instruction bloat / 工具权限冲突 / 需多轮结构化收集 / 需回退）。→ 状态机/分层是"遇到具体限制才付的成本"，过早引入会吃掉语音场景极敏感的延迟预算。<https://docs.livekit.io/agents/logic/workflows.md>
2. **"文档即状态"很脆：控制流一跳转，后面的 verb 永久不可达。** Twilio 官方警告："there are certain situations when the TwiML interpreter may not reach verbs in a TwiML document because control flow has passed to a different document... if a `<Say>` verb is followed by a `<Redirect>` and then another `<Say>`, the second `<Say>` is unreachable." → 把状态编码进可执行文档流，一旦跳转就丢失隐式顺序假设；这是**状态机刚性**的典型代价。<https://www.twilio.com/docs/voice/twiml>
3. **SIP 这一层状态机的复杂度本身就是反面教材。** 仅一个 INVITE 就有 client/server 两套事务状态机、`Timer A/B/D/G/H/I`、forking 产生的**多个 early dialog**，RFC 3261 正文一万五千行。业界普遍用 Twilio/LiveKit 把这层**封掉**而非让业务直接对接 —— 说明"正确的状态机"成本高且**正确性极难验证**。
4. **学术侧的状态机方案仍是"推断出来的、非持久的"，工程变复杂但远未可靠。** Agent-SAMA 的 FSM 从 UI 观察**实时构建**（非持久化、不可重放），恢复率 71.9%、AndroidWorld 63.7% —— 仍有约 36% 失败；SPlanner 63.8% 同样远未到可用，且都需额外的多 agent 协作/EFSM 构建维护开销。<https://arxiv.org/abs/2505.23596> · <https://arxiv.org/abs/2505.14141>

## 未找到 / 调研边界

- **未找到**：把"OS 级异步事件（通知、来电、权限弹窗、系统对话框、广播）"作为一等状态机驱动源的 OS/mobile agent 论文。
- **未找到**：OS agent 领域的 durable execution / checkpointer（进程被杀后续体恢复）研究。LiveKit `resume_false_interruption`、Gemini Live `sessionResumption` 是**最接近的工业实践**，但恢复的是"会话/播放位置"，不是"被挂起的工具调用续体"。
- **未找到**：Bland 等语音平台可引用的公开 call 状态事件参考页（Vapi 旧路径 `calls/call-events` 已 404）。
- **未核实故未引用**：搜索中出现若干 2026 年 NeurIPS 条目（"Trust, but Verify: Reliable Long-Horizon Mobile Agents Fully Solve AndroidWorld"、"AndroidReality"、"AndroidFlux"）—— 无法通过 arXiv API 核到编号，按"不编造"原则不写入结论；"Trust, but Verify" 精确检索只返回无关论文。
- Apple `AppIntents` 参数解析细节（`@Parameter`、`AppEntity` 查询、`perform()` 异步语义）在 JS 渲染子页，本轮未逐页核实。

## 对 SystemAgent 设计的直接推论

1. **聚合根 = run/session id**（与 `callSid` 同级）；状态与事件日志持久化，进程只是"当前拥有该 run 的执行器"。
2. **状态 = 显式枚举 + guard 条件 + 定时器**（借鉴 RFC 事务 Timer 与 Android Telecom 的枚举分层）。
3. **工具调用分两类**：blocking（普通函数）与 `NON_BLOCKING + scheduling`（等价 Gemini Live）——**默认应允许非阻塞**，否则"打电话"这类动作无处安放。
4. **回调至少一次**：事件带 sequence + 幂等键，终态兜底，并提供"用 id 反查当前状态"的 API（`Fetch a Call` 形态）。
5. **中断 = 广播控制帧 + 可撤销副作用清单**（Pipecat `InterruptionFrame` + `interruptible=False` 保留语义）。
6. **恢复 = 从事件日志重建状态机**，而非从栈帧恢复；连接/进程寿命视为**有界资源**（Gemini Live 10 分钟 + GoAway 预告）。
7. **能力发现要先探测状态**（`isAppFunctionEnabled` 形态）。
8. **状态机不替代模型**：SPlanner 的"EFSM 给骨架 + LLM 做 executor"比纯状态机或纯 ReAct 都强。

---

## 参考来源清单

**协议 / 平台一手文档**

- RFC 3261 — <https://www.rfc-editor.org/rfc/rfc3261.html>
- Android `Connection` / `Call` — <https://developer.android.com/reference/android/telecom/Connection> · <https://developer.android.com/reference/android/telecom/Call>
- Android FGS 后台启动限制 — <https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start>
- Android `AppFunctions` — <https://developer.android.com/ai/appfunctions>
- Apple `App Intents` — <https://developer.apple.com/documentation/appintents>
- Twilio TwiML / `<Dial>` — <https://www.twilio.com/docs/voice/twiml> · <https://www.twilio.com/docs/voice/twiml/dial>
- Twilio Call resource（状态值、`StatusCallbackEvent`）— <https://www.twilio.com/docs/voice/api/call-resource>
- Twilio Media Streams — <https://www.twilio.com/docs/voice/media-streams> · <https://www.twilio.com/docs/voice/media-streams/websocket-messages>
- LiveKit Job lifecycle / Turns / Workflows — <https://docs.livekit.io/agents/server/job/> · <https://docs.livekit.io/agents/build/turns/> · <https://docs.livekit.io/agents/logic/workflows.md>
- Pipecat Interruptions / Turn detection — <https://docs.pipecat.ai/pipecat/fundamentals/interruptions> · <https://docs.pipecat.ai/pipecat/learn/speech-input>
- Vapi Server URL / events — <https://docs.vapi.ai/server-url> · <https://docs.vapi.ai/server-url/events>
- Gemini Live API overview / session / tools — <https://ai.google.dev/gemini-api/docs/live> · <https://ai.google.dev/gemini-api/docs/live-session> · <https://ai.google.dev/gemini-api/docs/live-tools>

**论文（arXiv ID 已逐一核对）**

- AndroidWorld <https://arxiv.org/abs/2405.14573> · OSWorld <https://arxiv.org/abs/2404.07972>
- Mobile-Agent <https://arxiv.org/abs/2401.16158> · v2 <https://arxiv.org/abs/2406.01014> · v3/GUI-Owl <https://arxiv.org/abs/2508.15144> · E <https://arxiv.org/abs/2501.11733>
- AppAgent <https://arxiv.org/abs/2312.13771> · AutoDroid <https://arxiv.org/abs/2308.15272> · Voyager <https://arxiv.org/abs/2305.16291> · AutoManual <https://arxiv.org/abs/2405.16247>
- Agent-SAMA（FSM）<https://arxiv.org/abs/2505.23596> · SPlanner（EFSM）<https://arxiv.org/abs/2505.14141> · AgentProg <https://arxiv.org/abs/2512.10371>
- MobileUse <https://arxiv.org/abs/2507.16853> · UI-NEXUS/AGENT-NEXUS <https://arxiv.org/abs/2506.08972>
- GUI Agents: A Survey <https://arxiv.org/abs/2412.13501> · LLM-Brained GUI Agents: A Survey <https://arxiv.org/abs/2411.18279>
