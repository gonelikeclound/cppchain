# 03 · Tool 层详细设计（M3 实现）

> 承接 `02-arch-design/03-tool层架构设计.md`。记录实现细节与实验发现。

## 1. 实现总览

| 文件 | 职责 |
|---|---|
| `tool/tool.hpp` | `ToolError` / `ToolException` / `ToolContext` / `ToolSpec` / `arg<T>()` / `SchemaBuilder` / `make_tool()` |
| `tool/registry.hpp` | `ToolRegistry`：注册、按名查找、导出 OpenAI tools |
| `tool/selector.hpp` | `ToolSelector` 接口 + `AllToolsSelector` + `KeywordSelector`（CJK 2-gram + IDF） |
| `tool/tool_node.hpp` | `ToolNodeOptions` / `ToolNode`（批式并行 + 错误回喂） |
| `src/tool/tool.cpp` | `ToolSpec::to_openai()` |
| `src/tool/tool_node.cpp` | 执行、错误包装、分批并行 |

## 2. 关键设计

1. **显式 schema**：`SchemaBuilder` 写参数即得 JSON Schema；`arg<T>(args, "key")` 显式取值并返回
   `expected`（缺失/类型不符都有可读错误）——因为反射不可用（见 §3），且这样"每一步都看得见"。
2. **`ToolNode` 批式并行**：按 `max_parallel` 分批，批内 `std::async` 并发、批间串行 →
   **并发有界**且**结果按下标回填**（顺序恒等于 `tool_calls` 顺序，与完成先后无关）。
3. **错误回喂**：未知工具 / 无 handler / handler 返回 error / handler 抛异常 —— 四种都转成
   `ToolMessage{status="error"}` 回喂模型；`catch_errors=false` 时改为抛 `ToolException`。
4. **副作用标记**：`ToolSpec::read_only` 本层不拦截，只携带（留给 M7 审批中间件）。

## 3. 实验发现与修复（3 项）

| # | 发现 | 根因 | 处理 |
|---|---|---|---|
| 1 | **C++26 反射不可用** | `^^` 需 `-freflection`，且 `std::meta::identifier_of/parameters_of` 报 `is not a constant expression`（库是空壳） | 放弃自动推导，改显式 schema |
| 2 | **中文查询检索全军覆没**（返回 0 个工具） | ① 可检索文本只有 name+description，**参数描述里的中文没被检索**；② CJK 按单字切分，"天气"匹配不上 | ① 检索文本纳入参数名+参数描述；② CJK 改 2-gram |
| 3 | **高频词主导排序**（"get stock price" precision 0.38） | `get` 出现在 1/6 的工具名里，与稀有词 `stock` 同权 | **IDF 加权** `log(1 + N/df)` → precision 1.00 |

### 方法论教训（比上面三条更重要）

实验中途出现过"**压缩比 1444x**"的漂亮数字——真相是检索返回了 **0 个工具**（2 字节的 `[]`）。
**压缩比单独看毫无意义，甚至会把"把正确答案排除掉"包装成优化成果。**

→ 由此确立本项目的评测纪律：**检索类实验必须同时报 precision / recall，压缩比只能作为附带指标。**
这条已写入 `04-problems/01` 的正文，作为后续所有检索类实验的准则。

## 4. 验证结论

- `ctest`：**26 项工具层测试全绿**（规格/wire、tokenize、评分、IDF 回归、ToolNode 六类行为、并行时序与上限）
- demo：
  - `tools_basic`：闭环（模型发起 → ToolNode 执行 → ToolMessage 回填 → `validate_sequence` 通过 → 最终答复）+ 错误回喂
  - `tools_retrieval`：体积（N=1000 时 308 KB → 2.5 KB，125x）+ 质量（IDF 后 5/5 用例 precision=recall=1.00）
- 并行实测：4 个 60ms 工具、`max_parallel=4` → 61ms（串行需 ~240ms）；`max_parallel=2` → 103ms（两批）

## 5. 待办 / 开放点

1. `EmbeddingSelector` 预留未实现（需 embedding 端点）——与关键词检索做对照实验是下一步重点。
2. `select()` 目前 O(N) 全表扫描且每次重建 df；N≥5000 时 ~83 ms，需倒排索引/缓存。
3. `tool_search` 元工具、分层命名空间 —— 见 `04-problems/01` §6。
4. `read_only=false` 工具的审批路径留给 M7。

## 6. 新增内置工具：get_datetime / calculator / fetch_url（第一档）

> 承接 §2 的 `make_weather_tool` / `make_web_search_tool`，补全"自带工具箱"的第一档。
> 选型依据：零密钥 / 可离线单测 / 直插当前 research 主线（见讨论文档 01-项目基调）。

| 工具 | 实现 | langchain 参照 | 要点 |
|---|---|---|---|
| `get_datetime` | `src/tool/datetime.cpp` | 自研经典（langchain 无 canonical 内置） | 模型不知道"现在几点/星期几"；固定偏移时区（±HH:MM，**不依赖 tzdb**）；`DatetimeToolConfig::clock` 可注入 → 离线确定测试；`ttl=60s` |
| `calculator` | `src/tool/calculator.cpp` | langchain 内置 `Calculator` | 递归下降 + 直接求值，**非 eval**（无任意代码执行风险）；`-2^2=-4`（对齐 Python）、`^`/`**` 右结合、常量 pi/e、函数 sqrt/abs/pow/min/max/log…；长度/嵌套深度双防御（防 LLM 生成病态输入）；`calculate()` / `format_number()` 纯函数可单测 |
| `fetch_url` | `src/tool/fetch_url.cpp` | `WebPageContentExtractor` | 补全"搜索 → 打开详情页"闭环；GET 抓页 → `html_to_text`（剥 script/style/注释/标签、解码命名+数字实体、块级标签换行、压平空白）→ 标题 + 正文；`max_chars` 默认 4K/上限 20K 兜底上下文 |

**决策**：时区用固定偏移而非 IANA 名称（`tzdb` 可用性不可控）；内部统一 UTC、模型可传 `offset`。
**可测性**：纯函数暴露给单测，HTTP 路径复用 `FakeTransport` → 新增 **30 项测试全绿**、`ctest` 全量 210 通过。
**demo**：`examples/tools_frontline`（离线跑通：时间/时区、精确计算、除零错误回喂、抓页剥标签）。