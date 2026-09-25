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

## 7. 文件系统工具三件套：file_read / file_write / list_dir（第二档）

> 对应 `04-problems/03-工具副作用`。写工具是项目里**第一个 `read_only=false`** 的内置工具：
> 一进真实 Agent 循环就会被 `ToolApproval` 审批 + `SideEffectGuard` 防重放消费。

| 工具 | langchain 参照 | read_only | 要点 |
|---|---|---|---|
| `file_read` | `ReadFileTool` | ✅ 只读 | `max_chars` 截断（UTF-8 安全）、NUL 二进制检测、是目录/不存在给出可自愈错误 |
| `file_write` | `WriteFileTool` | ❌ **false（副作用）** | 覆盖/追加、自动建父目录、`max_write_chars` 上限；返回 `created` 与 `mode` |
| `list_dir` | `ListDirectoryTool` | ✅ 只读 | 条目按名称排序 + 类型/大小/修改时间；`max_entries` 兜底 |

**安全边界（本层最重要的设计）**：所有路径**相对沙箱根目录 `root_dir`** 解析，
`resolve_path_in_root()` 三重拒绝后才会操作：
1. **词法逃逸**：`..` / `a/../../b` 归一化后落在 root 外 → 拒绝（含"同名前缀兄弟目录"按 `/` 边界判定，不误伤 `x` 与 `x2`）；
2. **绝对路径**：`/etc/passwd` 直接拒绝（契约：一律相对）；
3. **符号链接逃逸**：`weakly_canonical` 解析后复查，root 内的链接指向 root 之外 → 拒绝。

写路径**先拒绝后执行**——越界试探不会落盘（有测试断言"外面文件不存在"）。

**踩坑记录**：
- 默认参数 `arg<T>(k, false)` 缺省时返回**空字符串**而非"未设置"，曾覆盖 list_dir 的默认路径 `.` → handler 需再判空。
- `lexically_normal()` 对 `root/.` 会**保留尾部斜杠**（`/tmp/x/`），导致与 root 本体比较不等 → 剥尾部空 filename。
- `file_time_type` 的 epoch 与系统时钟**不保证一致**（本机沙箱文件系统实测偏离 ~204 年）→
  modified 改用"同一时钟做差得 age → 系统时钟回推绝对时间"，与 epoch 基点无关。

**验证**：26 项新测试全绿（含越界三类拒绝、写后真实落盘、二进制检测、UTF-8 截断、副作用标记）；
`ctest` 全量 236 通过；`examples/tools_files` 离线 demo 通过（含越界/绝对路径被拒的展示）。
**开放**：真实 Agent 循环里接 `ToolApproval`/`SideEffectGuard` 的端到端验证留作下一步
（`side_effect_guard` 已用 send_email 覆盖该机制）。

## 8. arXiv 论文检索：arxiv_search（第三档）

> 对应 langchain `ArxivQueryRun`。research 场景王牌工具，免费无需密钥。

| 要点 | 说明 |
|---|---|
| 数据源 | arXiv 官方 **Atom API**（`export.arxiv.org/api/query`），返回 **XML 而非 JSON** |
| 解析 | **内置轻量 Atom 提取**（无第三方 XML 库）：按 `<entry>` 切块 → 取 title/summary/id/作者/分类/时间；`totalResults` 兼容 `<opensearch:totalResults>` 命名空间写法 |
| id 规范化 | `http://arxiv.org/abs/1706.03762v7` → `https://arxiv.org/abs/1706.03762`（去版本后缀 + 强制 https） |
| 检索语法 | 原样透传 arXiv 语法：`all:/ti:/au:/cat:` 前缀、`AND/OR/NOT`、引号短语；空格→`+`，其余百分号编码 |
| 摘要 | `default_summary_chars=800` UTF-8 安全截断，控制上下文占用 |
| 空结果 | `total=0` → 错误回喂"未找到匹配论文，请换词"，与 web_search 的"空结果不假成功"纪律一致 |
| ttl | 60 分钟（论文元数据更新慢） |

**共享提取（本档顺手做的重构）**：fetch_url/web_search 里各自实现的实体解码、UTF-8 截断、
空白压平（fetch_url 有过两套）重复到第三处（arxiv）之前，抽成 `include/cppchain/tool/text.hpp`：
`ci_find / trim_ascii / collapse_whitespace / normalize_space / utf8_truncate / decode_entities`。
fetch_url 改为复用（行为不变，测试全绿）；后续文本类工具直接引用。

**验证**：14 项新测试全绿（查询 URL 构造、命名空间解析、primary_category 优先、作者截断、
摘要 UTF-8 截断、空结果回喂、HTTP/网络错误、max_results 校验），`ctest` 全量 250 通过；
`examples/tools_arxiv` 离线 demo 通过；`run-demo.sh` 三个新工具 demo 已登记。