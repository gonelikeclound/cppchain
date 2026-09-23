# 02 · 架构设计（arch-design）

整体架构：分层、模块划分、核心抽象（如 Model / Message / Prompt Template / Output Parser / Tool / Agent Loop / Graph）之间的关系。

**规则**：先有 `01-discuss/` 中的讨论结论，再落这里；一篇文章对应一个架构主题或一个顶层模块。

## 文件列表

| 编号 | 文档 | 说明 | 状态 |
|------|------|------|------|
| 01 | [01-message层架构设计.md](01-message层架构设计.md) | M1 Message 层：类型族/ContentBlock/序列化契约 | ✅ 已定稿 |
| 02 | [02-chatmodel层架构设计.md](02-chatmodel层架构设计.md) | M2 ChatModel：OpenAI 兼容适配 + 异步回调内核 + 协程糖 | ✅ 已定稿 |