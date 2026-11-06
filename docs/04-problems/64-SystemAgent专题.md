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

### 第 23 轮（2026-11-04 · S10c：mock 个人设备事件）

- **用户选定边界**：个人设备 AIOS。把 mock 事件从云运维切到个人设备：
  通知 / 电量 / 日历 / 安全 / 健康 / 下载 / 媒体 / 屏幕 / 传感器 / 未知来源。
- **全局事件（进 SAD，5 条）**：
  - 妈妈微信提问 → `reply_and_remind`（读消息 → 草稿 → 发送 → 查日历 → 建提醒）；
  - 电量 15% ∧ 归途 → `power_saving`（省电/亮度/通知）；
  - 会议 10 分钟后 ∧ 通勤 25 分钟 → `leave_now`（查路线/提醒出发/闹钟）；
  - 安全 crit（异常登录）→ `security_lockdown`（锁屏/撤销会话/通知）；
  - 健康 crit（心率 150）→ `health_alert`（核查/联系紧急联系人/告警）。
- **本地事件（直达 Task Manager，4 条）**：下载完成 → `download_notify`；低电（非归途）→
  `power_saving`；好友消息 → inbox；媒体播放 → digest。**本地事件零 LLM**。
- **噪声/过滤**：900 传感器噪声丢弃、50 亮屏限流 45、20 群消息去重 19、5 非法 schema、
  1 未知来源（强制影子审计 → `miss_rate 0.41%`）。
- **结果**：985 事件 → 全局 5 / 本地 4 → **7 个任务**（5 次 SAD 理解 LLM，attempts=1）；
  Task Agent 执行个人设备工具：发消息/日历/提醒/省电/亮度/锁屏/健康告警/下载通知。
- **WebUI**：预设改为个人设备（妈妈微信提问、电量低+归途、会议+通勤、安全/健康告警、
  下载完成、设备休眠、传感器噪声、未知来源）；全局走 SAD/LLM，本地直达任务；
  输出台展示设备动作（音量/亮度/DND/熄屏/发消息/日历/提醒/省电/锁屏）。
- **实测**：首跑 17.4s（5 次 LLM），缓存 0.07s；微信提问模拟 → 5 个工具输出；无 JS 异常。
- **测试**：`sad_mock_tests` **12 条**（全局消息/本地下载/设备休眠/噪声/规格校验）；
  全仓 **1113/1113**；SA E2E 18/18 不变。

### 第 22 轮（2026-11-04 · S10b：分层修正——SAD 粗循环 / Task Manager / Task Agent）

- **用户纠正分层**：SAD 在最上层，是一个**粗略的固定 DAG**，遵循
  `事件 → 理解 → 任务 → 执行` 的循环；只有**需要全局协调**的事件才给 SAD；SAD 负责理解事件并
  **产生一个任务**；任务由专门 Task Manager 创建/监控/执行；**每个任务是独立 agent**。
- **据此重构 `sad_mock`（v2）**：
  - **SAD 粗 DAG（固定）**：`event → understand(LLM) → task_spec → submit → monitor → close`，
    monitor 可回到 understand（有界重规划，不重置预算）。
  - **理解层用真机 LLM**（`sad_understand.hpp`）：输出**任务规格 JSON**（goal/template/scope/
    domains/priority/budget/acceptance/constraints/dedup_key），模板必须来自预编排清单；
    校验不过回喂重试 ≤3，仍不过 fail-closed。**没有为任何具体事件/服务写特例**。
  - **全局/本地分流**：`deploy.failed` / `capacity.alert` / `security.alert(crit)` /
    `svc.api|gateway down` → SAD；其余（磁盘、deploy.finished、device.context、human.message）
    → 直达 Task Manager，不经过理解层（本地事件零 LLM）。
  - **Task Manager**：`submit(spec)` 按 dedup_key 创建/合并任务；任务状态 `completed|failed`；
    每个任务独立执行一个预编排 runbook（Task Agent 模板），产出工具输出。
  - **Task Agent**：本版用预编排模板（svc_restart / disk_cleanup / deploy_rollback /
    deploy_verify / security_incident / capacity_scale / focus_mode）；任务内动态规划留待后续
    （可复用 S9 的“一句话 → DAG”作为任务层 planner）。
  - 报告 v2：`events(layer/decision/task_id)` / `tasks` / `sad_traces` / `sad_dag` / `dags`；
    指标：global/local/sad_analyses/tasks_created/llm_calls/miss_rate。
- **WebUI 同步**：顶部默认渲染 **SAD 粗循环**（状态来自 trace，含“有界重规划”回环）；
  右上可切「任务 DAG」；事件列表显示 layer/decision/task；历史包含 SAD 轨迹与任务；
  模拟台区分全局（走 LLM 理解）与本地（直达任务）；输出台展示 Task Agent 工具输出。
  `sad_cgi` 增加 10 分钟报告缓存（页面首屏 `cache=1`），避免每次刷新都触发 4 次 LLM。
- **实测（真机 deepseek-flash）**：全量场景 985 事件 → 全局 4 / 本地 4 → SAD 理解 4 次（LLM 4）
  → **8 个任务**全部执行；首跑 14.3s，缓存命中 0.07s；全局模拟 2.1s（1 次 LLM），
  本地设备休眠即时直达 `focus_mode`（5 个工具输出）；无 JS 异常。
- **测试**：`sad_mock_tests` **11 条**（全局/本地分流、SAD 轨迹、任务输出、任务规格校验、
  设备直达、噪声丢弃）；全仓 **1112/1112**；SA E2E 18/18 不变。

### 第 21 轮（2026-11-04 · S10 WebUI：顶部 DAG + 四个页签）

- **用户要求**：SAD 页面，上面 DAG 图，下面分四个页签：事件触发列表 / SAD 处理历史 /
  事件模拟台 / 输出模拟台（展示 SAD 调用工具的输出，如调节音量、屏幕等）。
- **实现**：
  - `web/sad/index.html`（自包含）：顶部渲染**预编排 DAG 链**（step 工具 + args/output）；
    底部四页签——① 事件触发列表（984 条，decision/source/type 筛选，点击选中）② 处理历史
    （scenario/simulate 记录，点击回看 DAG/outputs）③ 事件模拟台（预设 + 表单，手动触发单事件）
    ④ 输出模拟台（tool / args / output 卡片）。
  - `examples/sad_cgi`：`action=scenario|simulate|history|clear-history`；事件 JSON 经临时文件
    传给子进程（不拼 shell）；历史原子写、上限 200 条；页面首屏 `record=0` 不重复追加历史。
  - `sad_mock` 扩展：DAG step 变为 `{tool,args,output}`；报告新增 `dags` 与 `events`
    （每条事件的最终处置）；新增 `simulate_one` + `--simulate-event`；新增设备 DAG
    `focus_mode`（set_volume / set_brightness / enable_dnd / screen_off / send_notification），
    由 `device.context{mode:sleep}` 触发。
  - `run-webui.sh` 增加 `sad_cgi` 构建与软链；SA 页面加「SAD 编排台」入口。
- **实测**：`/sad/` 首屏 984 事件、事件表展示 300 行、筛选 wake=5；「设备休眠」模拟 →
  `focus_mode` 5 个工具输出（音量 2 / 亮度 30 / DND / 熄屏 / 通知）；历史正常追加；
  无 JS 异常；生产 8000 端口 page/CGI 200。
- **测试**：`sad_mock_tests` **9 条**（新增 simulate 设备/噪声/报告处置）；全仓 **1110/1110**。

### 第 20 轮（2026-11-04 · S10a 落地：SAD 离线 mock 全过）

- **用户选择**：先做 mock，不接真实系统。
- **新增 `examples/sad_mock/`**（纯离线、确定性、无模型调用）：
  - **984 条 mock 事件**：900 噪声 + 50 健康正常（限流）+ 20 重复（去重）+ 5 非法（schema）
    + 真事件/专项钩子（服务 down、磁盘水位、发布失败、等待事件、容量告警、安全告警、
    人工消息、注入漏报）；
  - **L0-a**：schema / 未知来源 / 噪声类型 / 去重 / 限流；每次丢弃记录
    `{rule_id, version, reason}`；**L0-b**：确定性 mock classifier（生产换小模型）；
  - **白名单**（security./human./payment./health.）100% 进摘要层，crit 安全唤醒越过预算；
  - **精确 waits bypass**：`deploy.finished` 跳过 L0-b 直接进 L1a，且属于任务续跑、优先于打扰预算；
  - **L1a 规则 → 预编排 DAG**（注册表 5 个：svc_restart / disk_cleanup / deploy_rollback /
    deploy_verify / security_incident），运行时只路由不生成图；
  - **L1b**：按 DAG 合并唤醒 + 打扰预算（超预算进静默摘要）；
  - **影子审计**：25% 演示放大抽样 + 注入漏报 → `miss_rate`；`sad_mock_tests` 6 条。
- **实测**：候选 13、唤醒 5（安全 1）、预算静默 1、合并唤醒 1、去重 21、限流 45、噪声 900；
  5 个预编排 DAG 全部执行；11 条验收全过；`miss_rate 0.412%`；全仓 **1107/1107**。
- **产物**：`build/dev/examples/sad_mock --report=tmp/sad_mock_report.json`；
  报告含 metrics/checks/drops/wakes/runs/digest。

### 第 19 轮（2026-11-04 · S10 前置：事件分类、过滤、分层与需求）

- **用户要求**：先解决两个前置问题——①系统内会收到什么事件、如何过滤、是否需要多级 agent
  层层上报；②需求是什么、业界/开源是否有类似物。
- **结论**：
  - **事件过滤不用多级 agent**：L0-a 纯函数（结构/速率/时间窗/去重/源侧聚合）→ L0-b 廉价语义
    （小模型/分类器，禁用主模型，有界速率，判定可解释）→ L1a 规则命中（写 inbox，不唤醒）→
    L1b 编排唤醒（批量 + 预算）。**规则决定“要不要建任务”，模型只负责“填槽”**。
  - **多级只适用于决策/执行升级**，且推荐“单编排器 + 域专家 + 人”；升级 **≤1 次**、交接包结构化、
    **不重置预算**、不可逆动作不重复执行。
  - 必须是一等机制：**不可丢白名单**（平台/用户定义，SA 无权改）、精确匹配 bypass（活跃任务 waits）、
    **打扰预算**（每日上限/静默时段/超预算转静默摘要）、**漏报审计**（影子通道 + 事后回收 +
    no-rule 桶 → `miss_rate`）、每次丢弃可解释（`{rule_id, version, reason}`）。
  - **需求**：降噪/缩短 MTTR/统一 runbook/跨源关联/7×24/知识沉淀；验收指标 = 自动化率、MTTR、
    事件→动作延迟、误报/漏报率、唤醒/打扰次数、每事件成本、人工接管率。
  - **业界对照**：Alertmanager（去重/分组/抑制/路由）、PagerDuty/Opsgenie（升级策略）、
    StackStorm（sensors→rules→actions/workflows）、K8s controllers（reconcile）、AIOps/CEP、
    AI SRE agents（Azure SRE Agent / AWS DevOps Agent / Datadog Bits AI SRE / HolmesGPT / K8sGPT）、
    LangGraph/AutoGen/CrewAI（多 agent 编排）；多数只做一半——确定性工具没有智能，AI agent 工具
    缺少严格漏斗/预算/审计。SAD 的差异 = 确定性漏斗 + 静态 DAG + LLM gate + 唤醒账/审计。
- 详见 §5.7（事件清单、过滤规则、分层边界、需求/验收指标、升级契约、S10a 验收样例）。

### 第 18 轮（2026-11-04 · S10 设计讨论：常驻 SAD + 预编排 DAG + 多重拦截门）

- **用户提案**：SA 的另一个大作用是作为系统内的**常驻 agent（SAD, system_agent_daemon）**：
  ① 增加一个常驻 SA；② DAG 不在运行时生成，全部预编排；③ 在 DAG 上设置多重拦截门，
  避免系统海量事件淹没 SAD。
- **评价：方向正确（8/10）**，与 SA 主线一致：
  - 稳态事件重复/可枚举 → **预编排 DAG = 显式迁移表**：低延迟、零规划成本、可在加载期用
    `sa_guard`/M0 做静态校验（动态生成的图做不到同等程度的预校验）；
  - **多重门 = 研究稿的事件漏斗 + 打扰预算**：控制唤醒次数、token 成本与注意力；
  - 常驻形态是 **Reaper / L1 任务表 / 长时善后**的天然宿主；职责边界（SAD 编排、域执行）更清晰。
- **三处修正（已写入 §5.6）**：
  1. **“完全不生成”只适用于稳态**：改为“稳态 80–90% 事件走预编排 DAG；未匹配 / 高危 / 连续失败走
     受控升级”（待归类队列、低频规划 gate / 人审、诊断 DAG），否则新故障会“无图可用”或被静默丢弃；
  2. **门必须是一等公民**：L0 规范化 → L1 去重/合并 → L2 限流/分级 → L3 准入/优先级 → L4 咨询 gate；
     每道门有明确语义/配置/指标/失败策略，配 dead-letter 与高危旁路——防“过度过滤藏掉真故障”；
  3. **常驻工程问题不能低估**：幂等/at-least-once、checkpoint/journal、DAG 版本热升级、
     多实例 leader election、Reaper/背压、最小权限与副作用审批、唤醒账与可观测。
- **结论**：作为下一阶段主轴成立；落地形态 = **稳态静态 DAG + 受控升级 + 门流水线**，详见 §5.6。

### 第 17 轮（2026-11-04 · S9d：长文下载 → 分片读取 → 摘要 → 保存）

- **用户场景（最想验证的）**：「下载 歌德《少年维特之烦恼》，并做1000字的摘要，保存到文件」。
- **原失败**：Planner 用 `fetch_url` 传 `max_chars=60000`，但该工具上限 20000 → 工具报错；
  **upstream 失败后 synthesis 仍用模型训练知识写了 1021 字**（伪造“读过”），任务 Failed。
- **改动**：
  1. **`download_url` 工具**（`sa_tools.hpp`）：把 URL 完整内容下载到本地文件（默认上限 8MB），
     只返回 `{path, bytes, chars}`，不把正文塞进上下文；HTML 自动转文本、UTF-8 净化；
     路径只允许 `tmp/ / downloads/ / outputs/ / .sa_downloads/`，其余落到 `tmp/sa_downloads/`；
     `Idempotent`（幂等缓存），默认可用（不要求副作用开关）。
  2. **synthesis 自动分片读取**：上游 `download_url`/`file_read` 结果里有本地文件时，
     按 20k 字符切片（最多 12 片）→ 每片一次 LLM 提取要点（map）→ 汇总成摘要（reduce）；
     产出「分片读取 N 片 · M 字符」消息；facts 记 `chunks_read/chunk_llm_calls/source_files`。
  3. **上游失败禁止编造**：资料文件与工具结果都为空/失败时，synthesis **不调用模型**，
     直接报“拒绝用模型记忆冒充阅读结果”，任务 Failed。
  4. **保存到文件**：synthesis 可带 `args.save_to`（通用，非特色 case）；生成后写盘
     （需服务端副作用开关），facts 记 `summary_path`；路径同样限制在运行目录。
  5. **Planner 提示词**只加通用规则（长文档优先 download_url、下载后不要再加 file_read 重复读、
     分卷/分页要下全、要保存时填 save_to），**没有任何特定书名/URL 的说明**；
     `file_read` 单次窗口上限从 8000 提到 60000（仍可由分片/offset 控制）。
  6. **UI**：状态条/任务卡/详情 pill 显示「分片 N」「摘要 M 字」「已保存 <path>」；
     消息列表出现「分片读取」行与「已保存到」行。
- **实测（同一句话，走 WebUI CGI）**：5 节点
  `user → core → download_url → synthesis → world`；下载 259k 字符 → **12 片**、
  12 次分片笔记 + 1 次汇总（13 次 LLM）→ **1344 字**中文摘要 → 保存
  `tmp/sa_downloads/少年维特之烦恼_摘要.md`，任务 Completed；摘要开头标注来源文本。
- **测试**：全仓 **1101/1101**、E2E **18/18**（离线 fixture 与工具目录不受影响）。
- **诚实边界**：模型选到了 Gutenberg 的英译本（2527）而非德文原版，摘要内容据该文本分片笔记；
  分片上限 12 片（>240k 字符会截断并在消息中标注“仅前 12 片”）。

### 第 16 轮（2026-11-04 · S9c：让“LLM 生成”可辨识——去同构示例 + 规划来源/原文）

- **用户观察**：现在的 DAG 看起来不像 LLM 生成，倒像程序固定搞的。
- **诊断（一半对）**：
  - 「▶ 运行 demo」的 t1/t2 确实是 S1–S4 **手写常量 DAG**；
  - 一句话规划的 DAG 确实是模型现场生成（`planner=llm`、`model=deepseek-flash`；同句两次运行标题/检索参数会变，
    换问法会换工具），但**自由度被压得太小**：Planner `temperature=0`、Prompt 里 4 个 few-shot 示例
    （其中“检索+摘要”与常见问题同构，模型直接抄了示例的节点 id/形状）、校验器只接受窄拓扑、
    解释器只认识固定角色 → 看起来像模板。
- **改动**：
  1. **去掉具象 few-shot**：4 个场景示例替换为“字段示意（不要照抄结构）”的 schema 骨架；
  2. **Planner temperature 0 → 0.2**：允许在合法空间内选择不同结构/工具；
  3. **规划可追溯**：tasklog 记录 `plan_raw`（模型原始输出；fixture 模式存计划文件原文）与
     facts `planner_model` / `planner_attempts`；脚本 t1/t2 标 `planner=scripted`；
  4. **UI 区分来源**：任务卡徽标 `LLM / 脚本 / 计划文件`；详情 pill
     「来源 LLM · deepseek-flash（N 次尝试）」；「源码」标签切换扩展为
     `清单视图 → 原始 JSON → 规划原文`（规划原文即 plan_raw，脚本任务显示“无规划原文”）。
- **实测（去示例 + 0.2 温度后）**：5 条不同问法全部一次通过校验（attempts=1），节点 id/结构开始变化：
  - super-agent：`u1→c1→t1(arxiv)→s1(synthesis)→w1`；天气+建议：`u1→c1→t1(weather)→s1→w1`；
  - 电话+改口：`u1→c1→p1→c2→m1→w1`（7 条边，phone/msg 都写世界）；
  - 计算器：`user→core→calc→world`；纯消息：`u1→c1→m1→w1`。
  - UI 实测：任务卡显示 `LLM/脚本` 徽标；源码标签第三态显示模型原始 JSON；pill 显示来源与模型。
- **测试**：全仓 **1101/1101**、E2E **18/18**（不变；prompt 改动不进入离线 fixture）。
- **诚实边界**：图结构仍受校验器（安全约束）与解释器（固定角色语义）限制；这是“受约束的生成”，
  下一步若要更自由，需放宽拓扑并让 core/域节点也走运行时 LLM gate/agent loop。

### 第 15 轮（2026-11-04 · S9b：修复「只检索不总结」——新增 synthesis 节点）

- **用户反馈**：用 SA 运行「查找最新的 super-agent 内容，并生成摘要」——没有生成真正摘要的节点，也没有正确执行。
- **根因（实测 DAG）**：`user → core → [get_datetime, arxiv_search] → world`：
  1. IR 里**没有“生成摘要”的节点类型**；Planner 把“随后撰写中文摘要”写进 `core.task`，
     但 core 在解释器里只发 dispatch 消息、**不调用模型** → 实际只检索，任务却 Completed；
  2. core 有两条 dispatch 出边，同一条分诊消息被重复输出；
  3. Planner 自修复循环里的 `validate(p)` 没传工具白名单 → 清单外工具可能通过内层校验，
     外层才失败且不再重试。
- **修复**：
  - 新增 role=`synthesis`（最多 1 个）与 edge kind=`synthesize`（域/工具 → synthesis）：
    解释器收集上游工具结果 + 域消息，**真实调用 LLM** 生成摘要；产出 `summary` 消息、
    tasklog 的 `summary` 字段与 facts（summary_chars 按 UTF-8 字符计、llm_calls）；world 断言摘要非空。
  - 校验新增 P32（synthesis 必须有 synthesize 入边）/P33（边方向）/P34（≤1）/P35（task 必填）；
    Planner 提示词加 `synthesis` 角色、边类型、规则 8（摘要/总结/报告必须用 synthesis，
    不能写进 core.task）与示例 4。
  - **core 多出边只发一条分诊消息**；Planner 内层校验传入工具白名单，未知工具当场回喂重试。
  - UI：消息类型「摘要」+ 专属样式/徽章；任务 pill「摘要 N 字」；`synthesize` 边着色。
- **实测**：同一句话现在生成 5 节点/4 边：
  `user → core → arxiv_search → synthesis → world`，产出 ~1551 字中文摘要，
  `tools_used=[arxiv_search]`、`llm_calls=1`、`summary_chars=1551`，任务 Completed；
  页面显示「摘要」节点、摘要行与「摘要 1551 字」pill。
- **测试**：`sa_plan_tests` **21 条**（新增 synthesis 合法/缺输入/边方向/缺 task/多节点）；
  E2E **18/18**；全仓串行 **1101/1101**。

### 第 14 轮（2026-11-03 · S9：接入 cppchain 真实工具）

- **用户需求**：当前接入的工具太少，把 cppchain 的工具都接入，并且可以真实运行。
- **工具目录**（`examples/sa_demo/sa_tools.hpp`，Planner 清单与执行注册表**同源**）：
  - 默认 **17 个只读工具**：calculator / get_datetime / verify_sketch · 文件五件套
    （file_read / list_dir / file_grep / file_extract / file_sample，沙箱根=仓库根）· 旅行 DB 只读五件 ·
    get_weather / arxiv_search / fetch_url / read_image（需要 HTTP；实测北京天气返回真实数据）。
  - `--allow-side-effects`（CGI 侧：服务端 `SA_ENABLE_SIDEFX=1` + 请求 `sidefx=1` **同时满足**）
    额外加入 **file_write / submit_result / bash**，共 20 个；bash 走 restricted spawn executor
    （非真沙箱，仅兜底），文件工具全部限制在 root_dir 内。
- **IR 扩展**：新增 role=`tool` 节点（`tool` + `args`）。校验扩展：dispatch/report 支持 core↔工具、
  工具节点必须有 core dispatch、工具名必须在清单内（P30）、工具节点上限 4（P31）；
  同层多节点自动分行（避免 DAG 重叠）。Planner 提示词注入工具清单（名称/只读性/描述/参数），
  并给出 calculator 示例。解释器按拓扑序真实调用工具，结果写进消息、tasklog 的
  `tool_results` 与 facts（tools_available / tool_calls / tools_used）；world 节点断言工具成功。
- **安全边界**：默认只读；副作用必须**服务端显式开启**，避免局域网任意客户端直接跑命令；
  run_tool 注入 `ctx.executor`（bash 不再 fail-closed），effect_class 保留（Irreversible/Idempotent）。
- **UI**：新增示例 chip「算一算」「读 README」；「允许写入/命令」勾选框（服务端未开启时状态条明确提示）；
  任务详情 pill 与实际状态条展示使用的工具。
- **实测（真机 deepseek-flash）**：
  - 「算一下 (23*7+5)/2」→ calculator = 83；
  - 「读 README.md 开头」→ file_read 真读仓库文件；
  - 「北京今天天气」→ get_weather 返回真实天气；
  - `--allow-side-effects`：写文件真实落盘、bash `echo` 真实执行；
  - 默认模式下引用 file_write 的计划被静态校验拒绝（P30），零产出。
- **测试**：`sa_plan_tests` 16 条（新增工具节点合法/清单外拒绝/缺 dispatch/同层分行）；
  E2E **18/18**（calculator 离线 fixture=83、副作用默认隐藏）；全仓串行 **1096/1096**。
- **修复**：工具描述/结果按字节截断会切断 UTF-8 中文（nlohmann 抛 type_error 316）——
  改为 UTF-8 安全截断（prompt、工具结果、CGI 错误回显三处）。
- **未自动接入（诚实边界）**：MCP 动态工具（需要 server 配置与连接生命周期）、web_search
  （需 `WEB_SEARCH_API_KEY`）、read_skill/wiki_search（需 `WFR_SKILL_DIR`/`WFR_WIKI_DIR`，配置了才出现）、
  subagent 工具、examples 里的桩工具（缺 effect_class 声明，接入前需补齐治理标记）。

### 第 13 轮（2026-11-03 · S8：任务历史保留/删除 + 下载 + 更多示例）

- **用户需求**：① SA 页面保留过去历史，用户可删除，删除即持久删除；② 增加下载等其他 demo。
- **历史库**：`web/sa/sa_tasklog.json` 从「每次覆盖的结果文件」升级为**历史库**：
  - 合并规则：同 id 覆盖（脚本 demo 的 t1/t2 稳定 id，不会越跑越多）；一句话规划任务用
    `p<epoch-ms>` 唯一 id，每次运行**追加**；新任务在前（最近优先），上限 60 条；
  - 原子写（`.tmp` → `rename`），页面不会读到半截 JSON；
  - 脚本模式与规划模式都走同一合并函数（`load_history`/`merge_history`/`write_json_atomic`）。
- **删除（持久）**：`sa_cgi?action=delete&id=<task id>` —— id 白名单字符（≤64），从历史库移除并原子落盘；
  不存在则 `ok=false` 明确报错；页面每条任务卡右上角 `×`，删除后重拉历史（若删的是当前任务则自动选第一条）；
  运行中禁止删除（UI 侧拦截）。
- **并发护栏**：`sa_cgi` 运行路径加 `flock(web/sa/.run.lock)` 单运行锁——两个标签页同时点运行时，
  后到者明确返回「已有一次运行在进行」，避免 tasklog/进度互相覆盖（实测并发验证）。
- **下载（纯前端 Blob，无新增服务端端点）**：左栏「操作」行——「⬇ 下载当前」（当前任务
  dag+messages+facts JSON）、「⬇ 下载历史」（全部 tasks 的 sa-tasklog.json）、「删除当前」；
  任务卡上的删除从不易发现的 `×` 改为文字按钮「删除」（同一持久删除端点）。
- **更多 demo**：左栏示例 chip ×7 —— 电话+改口 / 只发微信 / 只打电话 / 电话→微信 / 微信→电话 /
  发给爸爸 / 约饭+改口，**一键运行**（填词并直接进入「LLM 规划 → DAG → 执行」，结果进入历史）；
  运行中重复点击被 UI 拦截（服务端 flock 兜底）。7 条均已用真机模型逐一跑通（单域 / 双域顺序 /
  改口改道三类形态）。
- **回执修正**：历史库多条任务时，CGI 回执的 `title/task_status/dag_*` 只取**本次新任务**
  （数组最近优先取首条；脚本模式只回传 t1/t2），修复「新任务已执行但状态条显示旧标题」。
- **验收**：E2E 扩至 **16/16**——离线 fixture 连跑两次断言历史保留>1 条；删除后服务端文件真实减少且
  重复删除报错；页面骨架新增「生成 DAG 并运行 / 下载历史」标记；全仓串行 **1092/1092** 不变。
- 备注：历史合并后，生成文件不再与 `sample-tasklog.json` 逐字节等价（第 12 轮的说法只适用于空的
  全新数据目录）；`sample` 继续作为页面静态回退数据源。

### 第 12 轮（2026-11-03 · S7：一句话 → DAG（真机 LLM Planner）→ 校验 → 执行）

- **用户澄清关键点**：SA 的核心不是「运行 DAG」，而是「**生成 DAG**」——能否根据用户一句话产出
  一张 DAG，然后把它跑起来。选定范围：**直接接真机 LLM Planner**。
- **分层**（LLM 只出现在规划 gate；域执行保持确定性、可回放）：
  1. **Planner**：`sa_demo --query="…"` → OpenAI 兼容端点（.env：`LLM_*`，本项目 deepseek-flash）
     产出**受限 IR**（role ∈ user/core/phone/msg/world；edge kind ∈ query/dispatch/report/control/world）；
     解析容忍 ```json 围栏与前后解释文字；不合法 → 把 issue 回喂模型重试（≤3 次），仍不过则 fail-closed 拒跑。
  2. **静态校验**（`examples/sa_demo/sa_plan.hpp`，纯函数）：P00–P29 —— 角色/kind 白名单、
     id 唯一、恰好 1 user/1 world、域节点必须有 core→域 dispatch、core 必须有 dispatch 出边、
     无环、所有节点在 user→world 路径上、control 只能 user→phone 且必须配 msg 域、规模上限（节点 3..12）。
  3. **通用解释器**：按拓扑序执行——user 消息 → core 分诊 dispatch → 电话域拨号/拒接重拨/接通
     （有 control 边则 barge_in 改道）→ 消息域 send_message → world 节点逐项查世界断言；
     消息与 tasklog/进度事件双写，UI 实时可视化；断言不过则任务 Failed。
  4. **入口**：CLI `--query` / `--plan-json`（跳过模型，离线可测）/ `--dry-run`；
     CGI `?planner=llm&query=<一句话>`（URL 解码在 CGI 做，query 经 **env `SA_QUERY`** 传子进程，
     不拼 shell，零注入面）；页面左栏新增「一句话」输入框 + 「生成 DAG 并运行」。
- **实测（真机 deepseek-flash）**：
  - 「给妈妈打电话…拒接再打一次，改口就发微信」→ 6 节点/6 边（含 control 边）→ 执行
    Completed、16 消息、7 里程碑、钟 27s；
  - 「给妈妈发微信说我晚上不回家吃饭」→ 4 节点/3 边 → 5 消息、钟 0s；
  - 电话-only（无 control）→ `c1 state=talking` 断言通过、6 里程碑。
- **测试纪律（R5）**：真机 Planner 不进 CI——新增 `sa_plan_tests`（12 条纯函数用例：合法计划、
  缺 world、环、未知角色、域缺 dispatch、core 无 dispatch、control 指向非电话、改口无 msg、
  phone-only 合法、布局、JSON 抽取）；E2E 扩至 **14/14**：离线 fixture 走完整校验+解释器
  （16 消息/7 里程碑/钟 27s）+ 坏计划（缺 world）必须拒跑且零产出；全仓串行 **1092/1092**。
- **诚实边界**：本轮的「智能」在规划 gate；域内工具序仍由确定性执行器（电话 FSM/消息发送）驱动，
  不是让 LLM 自由调工具。下一步可把域节点也交给 agent loop（真机），并让 Reaper/事件漏斗接管失败恢复。

### 第 11 轮（2026-11-03 · S6c：运行可视化——后端真慢跑 + 页面实时进度）

- **用户反馈**：「电话 mock 的例子太快了，看不到执行过程，mock 时候慢一点」。
  选定方案 = **后端真慢跑 + 实时进度**（非前端回放动画）。
- **sa_demo**：新增 `--progress=<JSONL>` 与 `--pace-ms=<0..5000>`：
  - 边跑边 append 结构化事件（逐行 flush）：`run start/done` · `task start/done` ·
    `dag`（节点×边先给页面）· `node`（active/done）· `msg`（与 tasklog 消息同口径）；
  - 每条 `msg` 后 sleep `pace-ms`（默认 0 = 全速，直跑/库测试不受影响）；
  - 事件内容与最终 tasklog **逐字对齐**（工具行由 args/输出统一成文；`clock_forward` 记在推进前）；
  - t1 DAG/t2 DAG 提到导演段前定义，实时进度与 S4 tasklog **共用同一份**（防漂移）。
- **sa_cgi**：`?scenario=all&pace=300` —— pace 只接受纯数字（≤5000，防 shell 注入），
  先 truncate `sa_progress.jsonl` 再 spawn（避免页面读到上一次残留）；回执新增 `pace_ms`/`progress`。
- **页面**：运行期间轮询 `sa_progress.jsonl`（200ms），节点 active 脉冲/已跑 done/未跑 pending，
  消息逐条追加，状态条显示「实时执行 · N ms/步」；只跟随本次运行的首个任务（t1 电话例）；
  回执到达后停止轮询并以 tasklog 重新装载（进度只是观测面，tasklog 仍是唯一真相）。
- **验收**：E2E 扩至 **12/12**——B 阶段 `pace=80` 并断言进度事件齐全（run/task/dag/node/msg、
  t1 `n_call` active、make_call/assert 消息）；C 阶段 `e2e=run&pace=80` 全通；
  生成 tasklog 与 `sample-tasklog.json` **逐字节等价**（diff 为空）。
- 体验：点「▶ 运行 demo」后能看见 用户 → 核心分诊 → 电话域拨号/拒接/重拨/接通 → 改口 →
  核心改道 → 消息域送达 → 世界断言 的完整过程（默认 300ms/步，约 6 秒）。

### 第 10 轮（2026-11-03 · S6b 修正：图源码 = DAG 定义，非仓库 C++ 源码）

- **用户纠正**：「源码」标签要的是 **SA 生成的 DAG 图的源码**（该图节点×边的定义本身），
  第 9 轮误做成「读仓库 C++ 源码」。
- **修正**：源码标签改为展示**当前任务 `dag` 定义** —— 「清单视图」（node `id [role] layer·row — label — detail`
  逐行 + edge `from --(label,kind)--> to`）⇄ 「原始 JSON」（`JSON.stringify(dag, null, 2)`）双视角；
  数据 = 页内 sa-tasklog 的 `cur.dag`，**纯前端、零外部读取**。
- **移除** `sa_cgi` 的 `?action=src` 端点（封闭名单读仓库文件那套整体删除，不留死代码/攻击面）；
  `sa_cgi` 回到纯「触发运行 + 回执 + run_meta」。
- **E2E 回归 11/11**：B 阶段 src 三检删除；C 阶段 `e2e=show` 扩展——切到「源码」标签并断言
  **图源码已渲染**（dagsrc 并入 render 判定，实测 ok）。
- 体验：右上面板 [节点图] / [源码] 互为「同一张图的两面」——渲染视图 vs 定义视图。

### 第 9 轮（2026-11-03 · S6b 右上「源码」标签页）

- **用户反馈**：「demo 的关系没看懂」→ 给右上面板加**标签页：`节点图 / 源码`**，源码页直读仓库源文件对照
  —— DAG 的每条边（分诊/改口/上报/写世界）都能在 `main.cpp` 导演段与 `samock.cpp` 假电话剧本里找到出处。
- **`sa_cgi` 新增只读端点 `?action=src&file=<key>`**（text/plain）：**封闭允许名单**六键
  main/samock/guard/cgi/e2e/page（用户输入只命中名单键、路径零拼接 → 局域网可达服务也不暴露越权读取）；
  **零副作用**（不 spawn、不动 run_meta）；run 路径向后兼容（`action` 缺省即 run）。
- **页面**：右上面板头变标签页（节点图 / 源码）；源码页 = 文件下拉 + `<pre>` 只读视图；
  非 WebUI 环境优雅降级提示。
- **实测**：`src&file=main` 200 含锚点 · `find=boom` 名单外拒 · **E2E 扩至 14/14**（新增 src 三检：
  内容锚点 / 零副作用（run_meta 不变）/ 名单外拒绝；A 阶段骨架标记加「源码」）。
- **实证排查两件（结论=无 bug）**：① `run_meta` 快照/还原逻辑实测 work（99→0 正确还原）；
  ② headless Chrome `e2e=run` 经隔离 `SA_DATA_DIR` 实测**只触发 1 次** run（非连发）。
- **run_meta 去噪**：历轮 E2E/手动触发在 `web/sa/run_meta.json`（gitignored 宿主观测）累积出不可追溯的
  18:xx 噪音簇，已重置为干净基线（total=0，页面「第 N 次」从 1 重新计数；原拟保留的 S5 早期 3 条亦
  被清理条件误伤一并清掉，如实备注——无业务价值损失）。

### 第 8 轮（2026-11-03 · S6 WebUI E2E 测试）

- **S6 全过（11/11）**：`run-sa-e2e.sh` —— SA 编排台 WebUI 端到端，驱动**真实服务**
  `python3 -m http.server --cgi`（同 run-webui.sh），**三阶段**：
  - **A 静态 HTTP**：`/sa/` 渲染骨架（SA 编排台 / 任务列表 / 运行 demo / DAG 四标记）+ 
    `sample-tasklog.json` 契约（schema=sa-tasklog/v1 · tasks=[t1,t2]）；
  - **B CGI 运行整路**：`scenario=all` 真实触发 sa_cgi→sa_demo —— JSON 回执（ok · elapsed_ms≥0 ·
    task_ids 含 t1/t2）、磁盘三产物（tasklog 2 任务 / run_meta total≥1 / run.log）、
    **run.log 含「S1 + S2 + S3 + S4 全部通过」=执行真发生**、确定性数值贯通
    （facts.milestones==7 · clock_end_sec==27 · core==2）、**第二次 run_total 递增**（append 语义）、
    `scenario=t1` → v1 明确拒答（含 v2 提示）；
  - **C 浏览器 DOM**：headless Chrome（`--dump-dom` × 页面内建 `?e2e=show/run` 钩子）——
    `show`：任务列表 + DAG 真实渲染（`data-e2e-render=ok`）；`run`：**真实点击「▶ 运行 demo」
    → fetch CGI →「运行完成」回执**（`data-e2e-run=pass`）。
- **隔离方案**：跑真实 `web/sa/`（保证页面↔CGI 数据同源），测试前后**快照/还原**三个运行态文件
  （sa_tasklog.json / run_meta.json / run.log），`trap EXIT` 清理服务器 / Chrome / 快照目录；
  已实测还原干净（run_meta 回 3、`.e2e_snapshot` 已删）。
- **同轮配套**：`sa_cgi` 加 **`SA_DATA_DIR`** env 旋钮（数据目录缺省 `<root>/web/sa`，可指临时目录做隔离）。
- **页面小修**（C 阶段暴露的瞬态竞态）：`runCgi` 成功/失败**直落 `data-e2e-run` 钩子**、
  `loadMeta()` 不再覆盖刚完成的「✓ 运行完成」、15s 看门狗兜底。
- **诚实边界**：E2E 覆盖 HTTP/CGI/磁盘效果 + headless Chrome 渲染/点击闭合；**非** playwright 人工
  视觉校验（环境只有 chrome --dump-dom，无 playwright），真手指视觉留给人工抽检。
- 下一步：**S7+** —— 事件漏斗 / L1 任务表 / Reaper 善后；**CGI v2**（单场景触发）并行候选。
- **脚本定位**：入口 `run-sa-e2e.sh`（仓库根，`./run-sa-e2e.sh [--no-build] [--no-chrome]`）；
  通过/失败计数打印、退出码=失败条数。

### 第 7 轮（2026-11-03 · S5 CGI 运行端点）

- **S5 全过**：浏览器 `GET /cgi-bin/sa_cgi?scenario=all` → `application/json` 运行回执；页面「▶ 运行 demo」
  按钮触发 → 重拉 tasklog/meta、保留当前选中。
- **`sa_cgi`（examples 目标，薄壳零 cppchain 依赖）**：解析 `QUERY_STRING`（scenario；
  **v1 仅 all** —— t1/t2 单场景触发 = v2，需把 S2/S4 导演段抽成可单独调用，已按契约明确拒答）
  → 定位 sa_demo 二进制（`/proc/self/exe` 真实路径同目录，或 `SA_DEMO_BIN` env 覆盖）
  → `chdir` 仓库根 → `popen` 子进程（stdout/stderr → `web/sa/run.log`）→ 测墙钟
  → 追加 `web/sa/run_meta.json`（runs 保留最近 50 条，gitignored 宿主观测、不进 tasklog）
  → 输出 JSON（ok / elapsed_ms / run_total / last_run_at / task_ids / tasklog 绝对路径）。
- **sa_demo 参数化**：`--tasklog=<path>`（默认 `web/sa/sa_tasklog.json`；CGI 传绝对路径），余下行为不变。
- **页面**：左栏加「▶ 运行 demo」+ `#run-status` 三态（运行中 / ✓耗时·次数·相对时间 / ✗失败原因）；
  `loadData(keepSel)` 重拉并保留当前选中；`loadMeta()` 显示 CGI 累计（上次 ms · 相对时间）；
  非 WebUI 环境（fetch /cgi-bin 404）优雅降级提示。
- **实测**：手动 CGI env 模拟 ✓（11ms · [t1,t2] · total 递增）；`python3 -m http.server --cgi` 全通路
  ✓（scenario=all → ok；scenario=t1 → 按 v1 契约明确拒答；/sa/、tasklog、run_meta 全 200）；
  run_meta 每次成功调用真实追加。
- **诚实边界**：sa_cgi 只做「触发 + 回执 + meta」，执行语义全在 sa_demo（全离线确定性，
  R5 纪律不碰真模型）。
- 下一步：**S6** —— 事件漏斗 / L1 任务表 / Reaper 善后；**v2** 单场景触发（抽导演段）并行候选。

### 第 6 轮（2026-11-03 · S4 SA 专用 UI 页落地）

- **S4 全过（17ms）**：SA 专用 UI 页 [`web/sa/index.html`](../../web/sa/index.html)（自包含单文件）。
- **页面布局（按用户规格原样）**：**左右两栏** —— 左 = **任务列表**（query + SA 回传消息 + 用户补充信息，
  组成一个完整任务）；右 = **任务详情**，分上下两部分 —— **上 = DAG 节点图，下 = 对话/消息列表，
  各占 50%、分隔条可拖动**（外纵/内横两条分隔，窄屏自动改轴）。
- **数据契约 `sa-tasklog/v1`**：`tasks[] = {query, user_supplements, sa_replies, facts,
  dag{nodes[layer×row/role/detail/state], edges[kind]}, messages[kind/node], milestones_summary}`。
- **demo S4 段（两条真实任务）**：
  - **t1 C7 简版**：6 节点 DAG —— 用户→核心①→电话域→核心②→消息域→世界（含`改口(barge_in)`控制平面边）；
    16 条消息 + 7 里程碑总结；核心 2 咨询（S2 断言已锁）；
  - **t2 消息直路**：单域快路（= 分诊退化结论，05 §2）—— 核心 1 咨询，4 节点链，无里程碑；
  - 事实（钟/里程碑/世界）**实时读 samock** 拼装，唤醒账用 S2/S4 断言锁定的常量；写
    `web/sa/sa_tasklog.json`（gitignore），页面回退内置 `sample-tasklog.json` + 拖放/导入 JSON。
- **页面实现要点**：主题 token 化（深色=套件基调，配浅色变体）；DAG = 内联 SVG
  （layer×row 网格布局、角色配色 currentColor、边 kind 着色 query/dispatch/report/control/world、
  marker 箭头 + 边标签）；**点节点 → 检查条显示执行细节 + 下方消息按节点筛选**；消息 7 种 kind 徽章
  （查询/SA/分诊/工具/里程碑/控制/断言）+ 虚拟钟 `t=Ns`。**零 CGI**：run-webui.sh 静态服务即看
  （`http://localhost:8000/sa/`）；「从页面触发运行」需 CGI run 端点，留作后续。
- 下一步：**S5+** —— 事件漏斗 / L1 任务表 / Reaper 善后；或给页面加「运行触发」。

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
- 下一步：**S4** —— SA 专用 UI 页（本轮已落，见下）。

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

### 第 2 轮（2026-11-03 · 拍板 cppchain 落地形态）

- **落点拍板**：基于 **cppchain** 做（非 DSH 宿主）；研究组 01–08 作语义基线，落点时映射到 cppchain 既有件。
- **组织形态拍板**：SA = **一组 agent**（核心 agent + 领域 agents），复用 cppchain 多智能体原语。
- **载体拍板**：SA 专属 **demo 目录** + **独立 `samock` so**（mock 代码全进 samock，参照 `sidefx_notify.so`
  先例：tests/plugin_demo 源 + `add_library(SHARED)` + env 目录加载）。
- 已产出：§六 落地方案骨架（demo / samock / 领域 agent 初集 / 落点映射），待逐条确认后开工。

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

web/sa/index.html                    # SA 专用 UI 页（自包含单文件；run-webui.sh → http://localhost:8000/sa/）
web/sa/sample-tasklog.json           # 内置样例（提交版）；sa_tasklog.json = demo 运行产物（gitignore）
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
| **S4** | **SA 专用 UI 页**（`web/sa/index.html`）：左任务列表 / 右详情 50% 可拖动（上 DAG 节点图下消息列表）；数据契约 `sa-tasklog/v1`；t1 C7 简版 + t2 消息直路两任务 | ✅ **2026-11-03 全过**（17ms） |
| **S5** | **浏览器触发运行**：`sa_cgi` CGI 端点（`/cgi-bin/sa_cgi?scenario=all` —— spawn sa_demo + run_meta 维护，v1 仅 all）+ 页面「▶ 运行 demo」按钮 | ✅ **2026-11-03 全过**（11ms 回执，http --cgi 全通路） |
| **S6** | **WebUI E2E**：`run-sa-e2e.sh` 三阶段 —— A 静态 HTTP（页面骨架+sample 契约）/ B CGI 运行整路（回执·磁盘三产物·run.log verdict·确定性数值·二次递增·v1 拒答）/ C headless Chrome（渲染 + 真实点击「运行 demo」→「运行完成」）；11/11 | ✅ **2026-11-03 全过** |
| **S6b** | **右上面板「源码」标签页（图源码）**：显示**当前任务 DAG 的定义**（节点×边清单 ⇄ 原始 JSON，纯前端）；**Round 10 修正**——撤去误做的「读仓库 C++ 文件」与 `sa_cgi?action=src` 端点；E2E 11/11（C 阶段断言图源码渲染） | ✅ **2026-11-03 全过** |
| **S6c** | **运行可视化（真慢跑 + 实时进度）**：`sa_demo --progress/--pace-ms` 边跑边写 JSONL（run/task/dag/node/msg），`sa_cgi?pace=` 控制节奏；页面轮询后节点逐个点亮、消息逐条出现；E2E **12/12**（B 阶段进度事件断言）；tasklog 与 sample 逐字节等价 | ✅ **2026-11-03 全过** |
| **S7** | **一句话 → DAG → 运行**：真机 LLM Planner（`--query` / CGI `?planner=llm&query=`）产出受限 IR → `sa_plan.hpp` P00–P29 静态校验（fail-closed，回喂重试≤3）→ 通用解释器按拓扑序调度电话/消息域 + 世界断言；页面输入框 + 实时 DAG；`--plan-json` 离线可测；`sa_plan_tests` 12 条 + E2E **14/14** + 全仓 **1092/1092** | ✅ **2026-11-03 全过** |
| **S8** | **任务历史 + 删除 + 下载 + 更多示例**：`sa_tasklog.json` 即历史库（稳定 id 覆盖 / 规划任务追加 / 最近优先 / 上限 60 / 原子写）；`sa_cgi?action=delete&id=` 持久删除；`flock` 单运行锁防并发覆盖；页面任务卡 `×`、下载当前/历史 JSON、3 个示例 chip；E2E **16/16** | ✅ **2026-11-03 全过** |
| **S9** | **接入 cppchain 真实工具**：`sa_tools.hpp` 工具目录（默认 17 只读；`--allow-side-effects` 加 file_write/submit_result/bash 共 20）；IR 新增 role=`tool`（tool+args），校验 P30/P31 与 core↔工具 dispatch；Planner 注入工具清单，解释器真实执行并把结果写入 tasklog；UI 加「算一算/读 README」与副作用开关；E2E **18/18**、全仓 **1096/1096**；实测 calculator/file_read/get_weather/file_write/bash 全部真跑 | ✅ **2026-11-03 全过** |
| **S9b** | **修复“只检索不总结”**：新增 role=`synthesis` + edge kind=`synthesize`，解释器把上游结果交给真实 LLM 生成摘要（summary 消息/tasklog summary/facts summary_chars·llm_calls），world 断言摘要非空；校验 P32–P35；Planner 规则 8 + 示例 4；core 多出边去重；Planner 内层校验带工具白名单；UI 摘要样式/pill/边色；`sa_plan_tests` 21 条、E2E 18/18、全仓 **1101/1101** | ✅ **2026-11-04 全过** |
| **S9c** | **让“LLM 生成”可辨识**：去掉 4 个具象 few-shot（改为 schema 骨架）、Planner temperature 0→0.2；tasklog 记录 `plan_raw` + facts planner_model/attempts，脚本 t1/t2 标 `planner=scripted`；UI 任务卡 LLM/脚本徽标、「来源 LLM · model」pill、源码标签第三态「规划原文」；5 条问法一次通过且结构/节点 id 开始变化；全仓 1101/1101、E2E 18/18 | ✅ **2026-11-04 全过** |
| **S9d** | **长文下载→分片读取→摘要→保存**：新增 `download_url`（全文落盘、只回路径/大小、UTF-8/HTML 处理、运行目录约束）；synthesis 自动分片 map-reduce（20k×12 片，分片笔记+汇总）；上游失败**禁止生成**（不调用模型）；`args.save_to` 保存摘要并记 summary_path；Planner 只加通用规则（长文下载、下载后勿重复 file_read、分卷下全、保存填 save_to）；UI 显示分片/摘要/已保存；同一句话实测 5 节点、12 片、1344 字、保存成功；全仓 1101/1101、E2E 18/18 | ✅ **2026-11-04 全过** |
| **S10** | **常驻 SAD 设计骨架**：稳态预编排 DAG + L0–L4 门流水线（去重/合并/限流/分级/准入/咨询 gate）+ DAG 注册表与路由 + 状态/journal/Reaper + 唤醒账 + 未匹配/高危受控升级；MVP S10a–S10e（详见 §5.6）；事件分类/过滤规则/需求/验收指标/升级契约见 §5.7 | 📝 **设计（2026-11-04）** |
| **S10a** | **SAD 离线 mock**：984 事件 → L0-a/L0-b/L1a/L1b → 5 个预编排 DAG；白名单/精确 bypass/打扰预算/影子审计；11 条验收 + `sad_mock_tests` 6 条；全仓 **1107/1107**（详见 §5.7.7、第 20 轮） | ✅ **2026-11-04 全过** |
| **S10a-UI** | **SAD 编排台 WebUI**：`web/sad/index.html` 顶部预编排 DAG 链 + 四页签（事件触发列表 / 处理历史 / 事件模拟台 / 输出模拟台）；`sad_cgi` scenario/simulate/history；`focus_mode` 设备 DAG（音量/亮度/DND/熄屏/通知）；`sad_mock_tests` **9 条**、全仓 **1110/1110**（详见第 21 轮） | ✅ **2026-11-04 全过** |
| **S10b** | **分层修正：SAD 粗循环 / Task Manager / Task Agent**：SAD 固定粗 DAG（事件→理解[LLM]→任务规格→提交→监控）；只有全局事件进 SAD，本地直达 Task Manager；理解层真机 LLM 输出受校验的任务规格；Task Manager 按 dedup_key 创建/合并并执行独立任务；页面顶部 SAD 循环（可切任务 DAG）；实测 985 事件→全局 4/本地 4→8 任务/4 LLM，`sad_mock_tests` 11 条、全仓 **1112/1112**（详见第 22 轮） | ✅ **2026-11-04 全过** |
| **S10c** | **mock 个人设备事件**：通知/电量/日历/安全/健康/下载/媒体/传感器；全局 5（微信提问→reply_and_remind、低电+归途→power_saving、会议+通勤→leave_now、安全/健康→lockdown/health_alert）进 SAD，本地 4 直达任务（零 LLM）；7 个 Task Agent 模板；页面预设改个人设备；实测 985 事件→全局 5/本地 4→7 任务/5 LLM，`sad_mock_tests` 12 条、全仓 **1113/1113**（详见第 23 轮） | ✅ **2026-11-04 全过** |
| S10+ | 常驻 SAD 落地（S10a–S10e，见 §5.6）；事件漏斗 / L1 任务表 / Reaper 善后 / 落地验收（C1–C12 抽角 C2/C3/C4/C6/C7/C8）；SA-CGI v2 单场景触发 | ⏳ 下一步 |

**落地前已拍板**：领域初集 = **电话域 + 消息域**（A 组）；demo 需要 **mock 电话**；
摆放 = `examples/sa_demo/` + `tests/plugin_demo/samock.cpp`；虚拟钟 v1 = **工具推进**的模拟钟（够用）。

### 5.6 S10 · 常驻 SAD（system_agent_daemon）设计骨架

> 2026-11-04 第 18 轮结论。目标：**系统内的常驻智能编排层**——稳态事件走预编排 DAG，
> LLM 只在声明的 gate 出现；多重门负责挡住事件洪峰；新场景通过受控升级处理。
> **事件分类/过滤规则/需求与验收指标的前置定义见 §5.7（第 19 轮）。**

#### 5.6.1 目标与非目标

- **目标**：常驻、事件驱动、可恢复、可审计；稳态 DAG **预编排**（不在热路径生成图）；
  多级门控；唤醒次数/token 成本可控；未匹配/高危事件受控升级；动作最小权限。
- **非目标**：不在热路径运行时规划 DAG；不让 LLM 直接执行领域动作；不承诺“任意事件全自动”。

#### 5.6.2 分层

```
事件源 → L0 规范化 → L1 去重/合并 → L2 限流/分级 → L3 DAG 路由/准入 → DAG 运行时（状态机 + 域调用）
                                                          │
                                                          ├─ 未匹配 / 高危 → 升级队列（规划 gate / 人审 / 诊断 DAG）
                                                          └─ 全程：唤醒账 + 审计 + Reaper
```

#### 5.6.3 事件 envelope（草案）

```json
{"schema":"sa-event/v1","id":"evt-…","source":"disk|svc|queue|…","type":"disk.full",
 "severity":"info|warn|crit","ts":"2026-11-04T12:00:00Z",
 "dedup_key":"disk:/var:full","payload":{…}}
```

- `dedup_key` 供 L1 去重/合并；`severity` 供 L2/L3；`source/type` 供 L3 路由。
- 事件 ID 幂等；至少一次投递 + 处理器幂等（见 5.6.6）。

#### 5.6.4 门流水线（L0–L4）

| 门 | 职责 | 语义 / 配置 | 指标 | 失败/兜底 |
|---|---|---|---|---|
| **L0** | 规范化 | schema 校验、字段补全、时间/来源标准化 | 非法事件数 | 进 dead-letter |
| **L1** | 去重 / 合并 | `dedup_key` + 时间窗；同键事件合并计数/摘要 | 去重率、合并率 | 保留首/末条 + 计数 |
| **L2** | 限流 / 分级 | 按 source/type/severity 的令牌桶；`crit` 可旁路但必须审计 | 丢弃数、旁路数、限额命中 | 进 dead-letter / 采样回放 |
| **L3** | 准入 / 优先级 | 每 DAG 并发上限、队列、饥饿保护、优先级 | 排队时长、拒绝数 | 背压，不静默丢 |
| **L4** | 咨询 gate | **只在 DAG 内声明**的 LLM 咨询点；token/次数预算 | 唤醒数、token、时延 | 超预算降级为规则/告警 |

> 纪律：**门必须可观测、可审计**；被丢弃/合并的事件进 dead-letter；按比例采样回放以量化
> **漏报率**（防“过度过滤藏掉真故障”）。高危事件走快车道，但旁路动作必须留痕。

#### 5.6.5 DAG 注册表与路由

- **事件类型/模式 → DAG id + 版本 + 参数绑定**（哪块磁盘、哪个服务）；路由规则是静态配置，
  不是 LLM 现场决策。
- DAG 预编排、加载期校验（复用 `sa_guard` P00–P35 / M0），版本化 + 热升级 + 回滚；
  同一事件类型可配置多个 DAG 版本做灰度。
- **未匹配事件** → 升级队列：低频规划 gate（离线生成候选 DAG）或人审；候选 DAG 必须过静态校验
  才能上线。
- **连续失败 / 高危** → 诊断 DAG（允许在 gate 处咨询 LLM 做根因分析），而不是在原 DAG 里硬塞。

#### 5.6.6 运行时状态机与恢复

- 事件循环 + 状态机；每次状态迁移写 journal/checkpoint；重启可恢复。
- **至少一次投递 + 幂等处理器**（事件 ID / dedup_key）；L1 去重与处理器幂等双层保险。
- **Reaper**：超时、卡死、长时间无进展的任务统一善后（取消/重试/升级/告警）。
- **背压**：队列水位触发 L2/L3 降级；不静默丢弃。
- **多实例**：按 source 分片或 leader election，避免重复处理。
- **唤醒账**：谁唤醒 SAD、为什么（事件/门/DAG/步骤）、花了多少 token/时间、结果如何。

#### 5.6.7 安全与治理

- 每 DAG 最小权限（工具/域白名单，复用 I1/I02）；副作用动作走审批/台账/审计。
- 门决策可审计（保留/合并/丢弃/旁路）；LLM gate 的输入/输出与预算留痕。
- DAG 版本、路由表、门配置都纳入配置管理，变更需评审（可复用 workflow IR 校验）。

#### 5.6.8 可观测指标

事件速率 · 去重/合并率 · dead-letter 数 · 高危旁路数 · 每 source 唤醒数 · 每 DAG 排队/执行时延 ·
LLM 调用/token · 重复处理率 · Reaper 干预数 · **漏报率（采样回放）**。

#### 5.6.9 MVP 里程碑（S10a–S10e）

| 里程碑 | 内容 | 验收要点 |
|---|---|---|
| **S10a** | 事件 envelope + 幂等键 + 审计/dead-letter 存储 | schema 校验、重复事件只处理一次 |
| **S10b** | L1–L3 门流水线 + 指标 | 事件风暴下唤醒数受控；丢弃可追溯 |
| **S10c** | DAG 注册表 + 路由 + 加载期校验 | 坏 DAG 拒绝加载；参数绑定正确 |
| **S10d** | 运行时 journal/checkpoint + 唤醒账 + Reaper | 崩溃恢复不重复/不丢关键事件 |
| **S10e** | 2 个真实 DAG（日常 + 告警）+ 故障注入 | 风暴/重复/乱序/重启全过；漏报率可量化 |

#### 5.6.10 开放问题

- 门的阈值如何标定？漏报率如何在线测量（采样回放的成本/隐私）？
- 未匹配事件的升级队列归谁（自动规划 gate / SRE 人审）？候选 DAG 的评审与灰度策略？
- DAG 版本与长时任务的兼容（运行中升级怎么办）？
- 多实例的 leader election 与事件分片；跨实例的幂等。
- 如何避免“为每个新故障写一个 DAG”导致注册表膨胀（模板化/参数化/组合子）。

### 5.7 S10 前置：事件分类、过滤与需求（2026-11-04 第 19 轮）

> 本节回答两个前置问题：系统里会收到什么事件、怎么挡；需求是什么、业界做到哪一步。
> 结论：**漏斗是多级的，智能不是多级的；升级是有界的，交接是结构化的。**

#### 5.7.1 事件分类与 envelope

按“来源 × 价值 × 是否可丢”分类（C6 的 4000 事件/夜是移动端样本，服务端同理）：

| 层 | 典型事件 | 频率 | 是否可丢 | 延迟敏感 |
|---|---|---|---|---|
| 基础设施 | CPU/内存/磁盘阈值、节点/pod 状态、网络抖动、证书到期、时钟偏移、备份结果、cron 失败 | 高 | 多数可聚合/可丢 | 中 |
| 服务/应用 | 错误率、延迟 SLO 破线、异常堆栈、发布/回滚、配置变更、依赖超时、队列积压 | 中高 | 不可静默丢 | 高 |
| 数据 | 主从延迟、数据质量、schema 变更、存储水位 | 中 | 不可丢 | 中 |
| 安全 | 登录失败风暴、权限变更、漏洞告警、入侵检测、审计异常 | 低中 | **不可丢** | 高 |
| 业务 | 订单/支付失败、用户投诉、工单 | 低 | 不可丢 | 高 |
| 人际/协作 | 聊天/邮件/工单/审批请求 | 低 | **不可丢** | 高 |
| 时间/治理 | 定时任务、维护窗口、成本异常、策略违规 | 低 | 部分可丢 | 中 |

事件 envelope（草案，对齐 `sa-event/v1`）：

```json
{"schema":"sa-event/v1","id":"evt-…","source":"svc|disk|queue|…","type":"svc.down",
 "severity":"info|warn|crit","ts":"2026-11-04T12:00:00Z",
 "dedup_key":"svc:api:down","correlation_keys":["svc:api","deploy:20261104-1"],
 "scope":"host|cluster|tenant","actionability":"auto|suggest|notify",
 "payload":{…}}
```

- `dedup_key`：L1 去重/合并；`correlation_keys`：窗口关联（如“电量低 ∧ 归途”）；
- `actionability`：决定“要不要建任务”；`severity`：决定限流/旁路/升级优先级；
- **源侧聚合**（C6 G-l）：SA 看到的应是“状态变化”，不是原始传感器/指标流。

#### 5.7.2 过滤流水线（4 级 + 旁路 + 白名单 + 预算 + 审计）

```
4000 事件/夜
  ├─ L0-a 结构过滤（纯函数）：schema/类型/来源/速率/时间窗/去重/源侧聚合
  │      → 60
  ├─ L0-b 廉价语义（小模型/分类器，禁用主模型，有界速率，判定可解释）
  │      → 10
  ├─ L1a 任务增强（规则命中 → 写 inbox，不唤醒）
  ├─ L1b 编排唤醒（需改图 → 批量合并，预算内）
  └─ 3~5 次编排调用/夜
```

（数字来自 C6 推演：4000 → 60 → 10 → 3~5；主模型直吃约 600 万 token/夜，漏斗后约 2.4 万，差 250 倍。）

| 层 | 判断什么 | 实现约束 | 失败方向/兜底 |
|---|---|---|---|
| **L0-a** | 结构合法、类型/来源、速率、时间窗、去重、源侧聚合 | 纯函数；微秒~毫秒级 | 丢弃可解释；白名单旁路 |
| **L0-b** | 廉价语义（“这条消息要不要回应”） | **禁用主模型**；令牌桶限速；输出 `{reason, confidence, model_version}` | 限速降级为“只走白名单” |
| **L1a** | 命中任务订阅/规则？写 inbox | 规则命中是纯函数；模型只“填槽” | 不唤醒；写摘要 |
| **L1b** | 是否需要改图/唤醒编排 | 批量合并 + 预算 | 超预算转静默摘要 |

一等机制：

- **规则 vs 模型**：规则决定“是否建任务”（可版本化/灰发/回滚），模型只在规则命中后填槽；
  规则库由人写/人审，SA 只提议（G-k）。
- **精确匹配 bypass**：命中活跃任务 `waits` 的事件直接进 L1a，跳过 L0-b。
- **不可丢白名单**：来电/短信/联系人消息/安全/财务/健康类，无论是否匹配订阅都进摘要层；
  白名单由平台/用户定义，SA 无权修改。
- **打扰预算**（一等资源）：

```yaml
interruption_budget:
  daily_max: 5
  quiet_hours: [23:00-07:00]
  on_exhausted: silent_digest
  priority_override: safety
```

- **漏报审计**：L0-a 丢弃事件 0.5% 抽样 + 白名单 100% 送 L0-b 复核；每周回看“用户事后手动
  处理了哪些事”；监控 no-rule 桶；产出 `miss_rate`（>0.5% 告警，>2% 自动放宽）。
- **纪律**：D-a 每级放大倍数有上限、单位成本递减一个数量级；D-b 每次丢弃可解释
  （`{rule_id, version, reason}`）；D-c 有兜底（白名单 + 影子通道 + 回收审计）。

#### 5.7.3 分层边界：漏斗 vs 升级（多级 agent 结论）

| | 过滤层（事件 → 候选） | 决策/执行层（候选 → 处理） |
|---|---|---|
| 该用什么 | 纯函数 + 小模型/分类器 + 规则 | 编排器 + 域 agent + 人 |
| 不该用什么 | 多级 LLM agent | 深链式多级 agent |
| 理由 | 成本/延迟/可解释性 | 上下文丢失/责任模糊/审计困难 |

推荐形态（不是“层层上报”，而是“边界分层”）：

```
L1 常规 DAG（确定性 runbook）
L2 SAD 编排器（单点，跨域协调/分诊）
L3 诊断/专家 agent 或人（仅在升级时出现）
```

- **过滤层不用 agent**：否则 4000 条事件在每层都调模型，成本和延迟不可控；
- **升级是有界的**：最多 1 次拓扑升级（`05` R6）；交接包带“已完成动作 + 剩余预算”；
  不重置预算；不可逆动作不重复执行；
- **多级 agent 只适合“知识/权限边界”不同**（域专家），不适合做事件过滤。

#### 5.7.4 需求清单

**要解决的问题**（按价值排序）：

1. 告警疲劳/信噪比低（C6 约 1:1300）；
2. MTTR 长（事件→诊断→动作→验证靠人串）；
3. Runbook 执行靠人肉且不一致、缺审计；
4. 跨源关联缺失（指标×日志×发布×依赖）；
5. 7×24 与事件洪峰；
6. 处理过的故障没有沉淀成规则/DAG。

**功能需求**：接入/规范化 → 去重/合并/关联 → 分级/限流 → 规则命中 → DAG 路由/参数绑定 →
执行（审批/幂等/审计）→ 验证/收尾 → 升级（诊断/人）→ 规则与 DAG 的版本化/灰发/回滚。

**非功能需求**：

- 延迟：L0-a p99 微秒~毫秒；L0-b 有界；唤醒按预算；
- 可用性：常驻、崩溃恢复、journal/checkpoint、至少一次 + 幂等；
- 安全：每 DAG 最小权限、副作用审批、门决策审计；
- 成本：每级单位成本递减、每事件 token 上限、影子通道 ≤ 编排预算 5%；
- 可解释：每次丢弃/唤醒都能回答“为什么”。

#### 5.7.5 验收指标

| 指标 | 定义 | 建议目标（首版） |
|---|---|---|
| 自动化处理率 | 无需人工介入完成的事件 / 总事件 | 先测基线，再设目标 |
| MTTR | 事件发生到恢复 | 相对基线下降 |
| 事件→动作延迟 | 事件 ts 到首个动作 | L0-a p99 < 5ms；端到端按 DAG 设 |
| 误报率 | 主动处理但用户不认可 / 处理总数 | 低（防烦） |
| `miss_rate` | 抽样发现该关注却被丢 / 抽样总数 | < 0.5% |
| 唤醒/打扰次数 | 每日主动触达 | 按打扰预算 |
| 每事件成本 | token + 时长 | 每级递减一个数量级 |
| 人工接管率 | 升级到人的比例 | 逐步下降 |
| 规则覆盖率 | `1 - no-rule 桶/候选` | 监控趋势 |

#### 5.7.6 升级契约（handoff packet）

```json
{"incident_id":"inc-…","from":"L1|L2","to":"L2|L3|human","signal":"S1–S8",
 "facts":[{"tool":"...","args":{…},"result_digest":"..."}],
 "actions_done":[…],"irreversible_done":[…],
 "remaining_budget":{"tokens":1234,"minutes":5},
 "escalations_used":1,"next_hint":"…"}
```

规则：**≤1 次升级**；不重置预算；不重复执行 `irreversible_done`；保留证据/观测摘要；
超时必须落到 `stuck`/`abandon` 路径（`02` §8.2 / `05`）。

#### 5.7.7 第一版 S10a 验收样例（mock 事件源）

> 首个垂直切片的具体系统边界待确认；先用 mock 事件源验证机制。

- **事件源**：`svc.health`（服务 down）、`disk.usage`（>90%）、`deploy.failed`、
  `noise.metric`（高频噪声，用于压力）。
- **场景**：1 分钟内注入 1000 条事件，其中 3 个真事件（服务 down / 磁盘水位 / 发布失败）。
- **验收**：
  1. 编排唤醒 ≤ 5；
  2. 白名单事件 100% 进入摘要层（即使未匹配订阅）；
  3. 每条丢弃含 `{rule_id, version, reason}`；
  4. `miss_rate` 可计算（注入 1 条“应关注但被规则漏掉”的事件，影子通道能发现）；
  5. 精确匹配 bypass 生效（活跃任务 `waits` 命中不经过 L0-b）；
  6. L0-a 在 10× 洪峰下 p99 不劣化，且能降级到白名单模式；
  7. 打扰预算耗尽 → 静默摘要；
  8. 运行中重启：checkpoint 恢复、不重复执行不可逆动作。
- **交付物**：envelope schema、门配置、DAG 路由表、一个日常 DAG + 一个告警 DAG、审计/dead-letter
  存储、指标报告。
- **实现状态（2026-11-04，S10a 全过）**：`examples/sad_mock/` 已落地（纯离线、无模型）：
  984 条事件 → 候选 13 → 唤醒 5（安全 1，预算静默 1，合并 1）；噪声 900、去重 21、限流 45、
  schema 2、未知来源 1；`miss_rate 0.412%`（影子抽样 243）；5 个预编排 DAG 全部执行；
  11 条验收 + `sad_mock_tests` 6 条全过；报告 `tmp/sad_mock_report.json`。
  **未覆盖**：10× 洪峰 p99 压测、运行中重启/checkpoint（留 S10d）。

#### 5.7.8 业界/开源对照

| 类别 | 代表 | 可借鉴 | 缺什么 |
|---|---|---|---|
| 告警去重/分组/抑制/路由 | **Prometheus Alertmanager** | dedup/group/inhibit/silence/routing（L1/L2 门） | 不会诊断/执行 |
| 值班升级/on-call | PagerDuty、Opsgenie、Grafana OnCall | 升级策略、分组、值班表 | 升级对象是人，不是 agent |
| 事件驱动自动化/runbook | **StackStorm**（sensors→rules→actions/workflows）、Rundeck、Ansible AWX、Argo Workflows | 事件-规则-动作、DAG、审批、审计 | 规则/动作靠人写，无 LLM 填槽/诊断 |
| 控制器/operator | **Kubernetes controllers/operators** | reconcile loop、level-triggered、声明式终态 | 无跨系统语义/诊断 |
| AIOps/事件关联 | Moogsoft、BigPanda、Datadog Event Management；Flink CEP/Esper/Drools | 关联、降噪、CEP | 多为相关性/规则，不是可执行 agent |
| AI SRE / 运维 agent | Microsoft Azure SRE Agent、AWS DevOps Agent、Datadog Bits AI SRE、PagerDuty AI、Rootly/incident.io AI；开源 **HolmesGPT**（Robusta）、**K8sGPT** | 诊断、根因、建议/执行 | 多缺严格漏斗/打扰预算/静态 DAG 审计 |
| 多 agent 编排框架 | LangGraph（supervisor/handoff）、AutoGen、CrewAI（hierarchical）、OpenAI Agents SDK（handoffs） | 编排/交接/子 agent 模式 | 没有事件接入、过滤、预算、审计 |
| 控制论/自愈 | MAPE-K（Monitor-Analyze-Plan-Execute-Knowledge） | 分层控制循环 | 工程化/LLM gate 需自补 |

**SAD 的差异化位置**：Alertmanager/StackStorm 式**确定性漏斗与 runbook** + **LLM 只在 gate**
+ **静态 DAG 加载期校验** + **唤醒账/打扰预算/漏报审计** + **有界升级**。业界通常是：确定性工具
没有智能，AI agent 工具没有严格漏斗——这是可做的空位。

## 六、关联

- 研究主线：[`docs/01-discuss/system-agent/`](../01-discuss/system-agent/README.md)（01–07 + `_research`）+
  整理稿 [08](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md) +
  doubao [设计档](../01-discuss/system-agent-doubao/system-agent-desigin.md)。
- 编号承接：problems 最大 63（[长上下文蜀山案例停置收口记录](63-长上下文蜀山案例停置收口记录.md)）；
  本篇=64，后续子篇顺延。
- 相邻面：`高可信/`（做得对不对）+ `主动智能/`（什么时候开始）= SA 相邻（SA 管"开始之后以什么形态运行"）。
- 内存关联：`cppchain-rsi-plan`（Agent 自进化）、`cppchain-personal-agent-plan`。