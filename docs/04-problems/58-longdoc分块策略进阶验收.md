# 58 · longdoc 下一刀：分块策略进阶（章节正则 / 固定窗口）

> **链路**：arch 28（problems/57，结构骨架+倒排+按需深读，分块第一版=文件）→ 本档补**分块策略**。
> 目标：单个大文件（一部长篇/一份长文档/整个日志）能按「章节」或「固定窗口」切成多个单元，
> 让结构优先/按需深读真正作用于**一份长文本内部**，而不只是"目录下多文件"。

## 一、问题

arch 28 第一版 `unit_of = file`（目录下每个文件=一个单元）。但超长内容的常见形态是**单个大文件**：
一部小说合并成一份 txt、一份长合同、一天的日志。这时"一个文件一个单元"等于没分块——单块还是超长，
骨架和检索都无从谈起。

## 二、方案

ingest 节点运行时读 `@config.longdoc_unit_of`（缺省 `file`）分派三种策略（config 驱动，不改 IR）：

| 策略 | 切法 | 参数 |
|---|---|---|
| `file` | 整文件=1 单元（原行为，向后兼容） | — |
| `chapter` | 按章节标题行切块：行首「第」+ ≤8 字符内出现 回/章/节/卷/部（如 `第1回 Alpha Story`、`第一百零八回 双女四美`）；无标题 → 退化整文件 1 块（fail-open 不丢数据） | — |
| `fixed` | 固定字符窗口切块，带 overlap（UTF-8 安全：只切在字符边界） | `longdoc_chunk_chars`（缺省 4000）、`longdoc_chunk_overlap`（缺省 200） |

- **索引 v2**：units 每项加 `offset`（块在文件内的字节起点）；`bytes` 语义从"文件大小"变为"块长"。
  `meta.unit_of` 记策略名。旧索引（无 offset）retrieve 兼容为整文件读取。
- **retrieve 按块读**：`[offset, offset+bytes)` 区间读，不再整文件读后再截——真正"按需加载命中块的
  局部窗口"，命中块之外的原文字节一个都不进内存。

## 三、实现（src/workflow/node_types.cpp longdoc 段）

- 助手：`longdoc_seq_len`（UTF-8 首字节定序列长）、`longdoc_char_starts`（字符→字节偏移表）、
  `longdoc_chapter_heading`（章节标题判定，字节模式匹配 回/章/节/卷/部）、
  `longdoc_chunk_file/chapter/fixed`、统一入口 `longdoc_chunks_for`（未知策略 fail-closed）。
- ingest：每文件 → 按策略切块 → **每块单独产骨架**（模型档/规则档都按块给，keys 更聚焦）→
  units 带 `offset`；`id`：file=文件名，多块=`<fname>!<n>`（整文件退化的单块 id 仍=文件名）。
- retrieve：`offset`/`bytes` 区间读（旧索引 offset 缺省 0 兼容）；hits 增 `offset` 字段可追踪。

## 四、测试（tests/wf/longdoc_test.cpp 新增 4 例）

| 测试 | 断言 |
|---|---|
| `fixed_window_chunks_and_range_retrieve` | 300 字文件固定窗 → 多块、偏移单调/首 0；query 命中块只读该块范围（不含其它窗的词）；hits.offset 与索引对齐 |
| `chapter_headings_split_and_title` | 「第X回 + 英文标题」三回 → 3 块、title=标题行、id=`novel.txt!N`；query 命中对应回且只读该回 |
| `chapter_no_heading_falls_back_whole` | 无标题文件 + chapter → 1 块、id=文件名、offset=0 |
| `fixed_window_boundaries_utf8_safe` | 纯中文 7 字窗必然横跨多字节字 → 每块起点字节**不是续字节**（UTF-8 边界安全） |

**顺带修存量真 bug**：`longdoc_tokenize` 里 `char c == '，'` 这类多字节字面量比较**恒为 false**
（char 容不下多字节）——中文标点从没真正分词过，是死代码 + `-Wpedantic/-Wtype-limits` 告警源；
按注释本意删掉无效比较（中文无空格整段一 token、双向子串命中兜底），告警清零。

## 五、结果

- 全仓 **920 全绿**（916 + 4）；longdoc 测试 8→12。
- arch 28 §四 边界更新；索引向后兼容（旧 file 索引照旧可用）。

## 六、遗留 / 下一刀

- **向量臂**（复用个人 Wiki 向量：泛问"第一回讲了什么"这类无专名问题靠 blurb 语义检索）、
  命中子集升级 merge —— 仍未做。
- 骨架提取模型档真机质量（blurb/keys 准不准）—— 真机试。
- 检索排序 BM25 / 向量 hybrid —— 押后。