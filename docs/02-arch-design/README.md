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
| 12 | [12-节点接口与注册表.md](12-节点接口与注册表.md) | 节点接口 `NodeType` + 工厂动态注册（内置 / 动态库 import）+ **统一日志**（history 改为日志投影）| ✅ ①② 已实现（注册表 + `edges()` 收敛）；③–⑤ 仍设计 |