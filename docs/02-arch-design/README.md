# 02 · 架构设计（arch-design）

整体架构：分层、模块划分、核心抽象（如 Model / Message / Prompt Template / Output Parser / Tool / Agent Loop / Graph）之间的关系。

**规则**：先有 `01-discuss/` 中的讨论结论，再落这里；一篇文章对应一个架构主题或一个顶层模块。

## 文件列表

| 编号 | 文档 | 说明 | 状态 |
|------|------|------|------|
| 01 | [01-message层架构设计.md](01-message层架构设计.md) | M1 Message 层：类型族/ContentBlock/序列化契约 | ✅ 已定稿 |
| 02 | [02-chatmodel层架构设计.md](02-chatmodel层架构设计.md) | M2 ChatModel：OpenAI 兼容适配 + 异步回调内核 + 协程糖 | ✅ 已实现 |
| 03 | [03-tool层架构设计.md](03-tool层架构设计.md) | M3 Tool：显式 schema + 可插拔检索 + ToolNode 并行 | ✅ 已实现 |
| 04 | [04-embeddings层架构设计.md](04-embeddings层架构设计.md) | 向量能力：DashScope 适配 + EmbeddingSelector（卡位问题的解法②） | ✅ 已实现 |
| 05 | [05-structured-output架构设计.md](05-structured-output架构设计.md) | M4 结构化输出：两策略 + 最小校验 + 回喂重试 | ✅ 已实现 |
| 06 | [06-state层架构设计.md](06-state层架构设计.md) | M5 状态层：reducer/add_messages + Checkpointer + Store | ✅ 已实现 |
| 07 | [07-graph引擎架构设计.md](07-graph引擎架构设计.md) | M6 Graph 引擎（极简）：顺序 super-step + 快照恢复 | ✅ 已实现 |
| 08 | [08-agent循环架构设计.md](08-agent循环架构设计.md) | M7 Agent 循环：middleware + 三个卡位问题落地 | ✅ 已实现 |
| 09 | [09-多智能体架构设计.md](09-多智能体架构设计.md) | M8 多智能体：subagent-as-tool + orchestrator-worker(串行) + handoff | ✅ 已实现 |
| 10 | [10-m9收官设计.md](10-m9收官设计.md) | M9 收官：三中间件做深（token 阈值/成本量化/审批记忆化）+ 混合检索 + artifact 落盘 | ✅ 已实现 |
| 11 | [11-组件化与命令行agent.md](11-组件化与命令行agent.md) | 拟定 M12：**agent 循环 = 图上的回边**（对齐 langgraph ReAct）+ history 节点 + 组件按名引用 + 工作路径续跑 | 🟡 设计（落盘机制已验证）|
| 12 | [12-节点接口与注册表.md](12-节点接口与注册表.md) | 节点接口 `NodeType` + 工厂动态注册（内置 / 动态库 import）+ **统一日志**（history 改为日志投影）| ✅ ①–⑤ 全部实现（注册表 / `edges()` / `session_log` / replay 投影 / A0+A1 导入枚举） |
| 13 | [13-graph运行时补全设计.md](13-graph运行时补全设计.md) | **M10 运行时补全**：包队列执行器 + Streaming / Interrupt / Send / 时间旅行；checkpoint 的 next 升为包队列 | ✅ 已实现（438 全绿 + 4 离线 demo） |
| 14 | [14-wfr命令行工具与调试可视化设计.md](14-wfr命令行工具与调试可视化设计.md) | **wfr 系列**：命令行运行器（按名执行）+ trigger 可选 / import 复用 + 单 HTML 调试台；`.wfr/` 统一元数据目录 | ✅ 已实现（438 全绿 + `wfr_demo`） |
| 15 | [15-思考模型推理回传与历史压缩策略设计.md](15-思考模型推理回传与历史压缩策略设计.md) | **wfr-㉙**：历史压缩三档（full / drop_round / keep_evidence）+ `ReasoningEcho` 出站三档 + model 节点厂商参数透传；chat2 对照 workflow | ✅ 已实现（495 全绿 + 真机三档对照） |
| 16 | [16-loop节点与迭代分组设计.md](16-loop节点与迭代分组设计.md) | **wfr-㉚**：`loop` 容器节点（body 子树 + `_continue`/`_break` + `max`/`on_max`）取代 label+goto；调试台按迭代分组 | ✅ 已实现（517 全绿 + 真机） |
| 17 | [17-历史消息处理机制设计.md](17-历史消息处理机制设计.md) | **wfr-㊵**（本专题）：messages 的 **op 语义**（fold/transform，可持久化的派生视图）+ `summarize` / `refine_history` 两个节点 + 规则引擎与工具类别 | ✅ 已实现（570 全绿 + 真机三方向对照） |
| 18 | [18-副作用防护接入Workflow执行路径设计.md](18-副作用防护接入Workflow执行路径设计.md) | **wfr-㊷**（设计存档·二版）：审批=独立节点 `approve_tools` / 防重放幂等=执行点内部（以会话日志为事实源）/ 崩溃缺环=`ToolSpec::probe` 兜底 / 记录以日志为准 | ✅ 已实现（615 全绿 + 真机三场景，见 problems/21） |
| 21 | [21-多Agent角色化PEV设计.md](21-多Agent角色化PEV设计.md) | **wfr-㊾ · 多 Agent（从设计档 A 起步）**：角色=独立身份(system_prompt)+输入隔离；Planner/Executor/独立 Reviewer/final 用现有节点组装；Reviewer 不看 Executor 思考只看记录+判据；分期 ① 串行 mch 对照 ② Send 并行 Executor ③ 交接式 | ✅ 批①（681 全绿+problems/29）、批②（693 全绿+problems/30）、批③（702 全绿+problems/31）全落地 |
| 20 | [20-长程任务任务档案与批还原设计.md](20-长程任务任务档案与批还原设计.md) | **wfr-㊿ · 长程任务（定架构·待实现）**：任务=.wfr/tasks/<task>.json 档案（goal+steps+批进度+批摘要）；`task` begin/end 节点做批还原与写回；时间感注入 `{batch_no,date}`；批内复用 PEV 环；分期 ① 档案+lot ② 批间可改 ③ 调度押后 | ✅ ① ② ③ 已实现（批① 档案+lot、批② revision-fork+复查、批③ cron；真机 C1 分批案例见 problems/28） |
| 19 | [19-复杂任务规划执行验证环设计.md](19-复杂任务规划执行验证环设计.md) | **wfr-㊹**（定架构·待实现）：任务级 P-E-V 环（计划=一等公民的 state 数据 / verify 节点 probe 判据 / 步骤边界 interrupt / 两级回退不做命令反做）+ 新节点 plan·verify·plan_review | ✅ ①–④ 全部实现（L1/L2/L3 + 批④ 中断回退；见 problems/24/25；674 全绿） |
| 22 | [22-长程任务实现全景.md](22-长程任务实现全景.md) | **wfr-㊿ · 实现全景（代码实证）**：档案/会话分工、数据形态、一条批运行时路径、跨批机制五件、调度、自进化接点、与 PEV/mch 位置、批③ 交接件衔接 | ✅ 已建档（2026-10-16） |
| 23 | [23-治理全链workflow设计.md](23-治理全链workflow设计.md) | **wfr-㊹ 治理（problems/33）**：governed 单 workflow 完整实现治理思想——UNVERIFIED 双档 / _timeout / _cancelled / effect_class+自检，终态四档穷尽 | ✅ 已实现（720 全绿，problems/33） |
| 24 | [24-proxyNode子workflow调用设计.md](24-proxyNode子workflow调用设计.md) | **wfr-㊿ proxyNode（arch 21 预留原语落地）**：`subworkflow` 节点运行期调用子 workflow（隔离执行）——sync 子结果提升回传 / detach fire-and-forget 落盘审计 / copy_state+input 构造子 state / 编译期装载子 IR（fail-fast）+ 环与深度防护 | ✅ 已实现（851 全绿，problems/47） |
| 25 | [25-声明式配置三形态设计.md](25-声明式配置三形态设计.md) | **P1.5（借鉴 miclaw-rs `crates/config`）**：`declarations.configs` 声明式配置——命名空间点分引用（仅声明名导航）/ target 平铺 / `${workdir}` 运行变量 / 编译期启动即炸；在**全局 config 表**上生根（不做逐节点 config 对象） | ✅ 已实现（861 全绿，problems/48） |
