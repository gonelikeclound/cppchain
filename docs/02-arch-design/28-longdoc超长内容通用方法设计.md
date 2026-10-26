# 28 · longdoc：应对超长内容的通用方法（结构骨架 + 倒排 + 按需深读）

> **定位（用户定调）**：蜀山传（还珠楼主《蜀山剑侠传》309 回）只是**测试物**，不是目标。
> 本专题的目标是借蜀山实现一套**应对超长内容的通用方法**——任意超长文本（小说/法律文档/代码库/日志/财报）
> 都能快速处理：先掌握结构、按需深入，不全量烧 token。
> 本档落**机制层**：三个与语料无关的通用件 + 通用索引格式；蜀山只是第一个验证案例。

## 一、核心范式：全文 = 需要导航的数据结构，不是"一份可全量进上下文的文本"

应对超长内容的通法三层（语料无关）：

| 层 | 做什么 | 成本 |
|---|---|---|
| **① 结构优先** | 全文切"结构单元"（小说=回/块；文档=节/段；代码=文件/函数；日志=时间窗），每单元只产**轻量骨架**（标题 + 一句话 + 出现的专名/主题） | O(单元数)，非 O(全文 token) |
| **② 倒排索引** | 从骨架提取可检索键 → 「键 → 单元集合」倒排 + 「单元 → 原文路径」映射 | 一次构建 |
| **③ 按需深读** | 任意问题 → 键命中 top-k 单元 → **只加载命中单元的原文局部窗口**（带预算）→ 回答带引用 [单元 id] | 每问小窗口 |

可选深路径：需要全局整合时，对命中子集升级为 merge（原 Map/merge 保留，不默认）。

## 二、通用机制件（`longdoc` 三件，零蜀山字样）

### 1. 索引格式 `longdoc.index.json`（与语料无关的抽象）
```jsonc
{
  "meta": { "root": "<单元原文根目录>", "unit_of": "<分块策略名，第一版=file>", "built": "<时间>" },
  "units": [ { "id": "<单元名>", "title": "<标题>", "blurb": "<一句话骨架>",
               "keys": ["<专名/主题>", "…"], "path": "<相对 root 的原文路径>", "bytes": <原文字节> } ],
  "keys": { "<键>": [<unit 下标>, "…"] }      // 键 → 单元倒排
}
```

### 2. 节点 `longdoc_ingest`（建索引，单相位）
- 输入：`@config.longdoc_root`（单元原文目录，**每个文件=一个单元**，第一版分块策略）+ 可选 `model`（骨架提取器）。
- 单遍扫描：每单元读原文 → 产 `{blurb, keys}` → 汇总写 `@config.longdoc_index`（缺省 `<root>/../longdoc.index.json`）。
- **双档骨架提取**：
  - 模型档（有 model）：每单元一次轻量调用，提示词固定「给这段一句话骨架（≤60字）+ 列出出现的专名/主题（逗号分隔），JSON {blurb, keys:[]}」。
  - 规则档（无 model）：blurb = 首行/首 120 字符；keys = ASCII 词（空白/符号分词，代码/英文文档有效）；中文语料 keys 为空（仅结构导航，倒排留模型档）。fail-open，不阻断建索引。
- 产出：results.{units, keys, index_path}；索引整文件原子写。

### 3. 节点 `longdoc_retrieve`（按需检索 + 切片加载）
- 输入：`@config.longdoc_index` + `@config.query` + `@config.longdoc_topk`（缺省 3）。
- 匹配：query 分词（空白/常见标点）→ 每词在 `keys` 子串匹配（词含于键 或 键含于词）→ 命中单元集合 → 按「命中键数 × IDF」排序 → top-k。
- 切片加载：对 top-k 单元读原文（相对 root），每单元截断到预算（`@config.longdoc_slice_chars`，缺省 8000 字符，UTF-8 安全）。
- 产出：results.hits = [{id, blurb, keys, text(截断), bytes}], results.note（索引缺/查空时 fail-closed 或空命中说明）。

### 4. 查询 workflow 模板 `longdoc_ask`（share/wfr/workflows/）
```
retrieve(@config.query) → model(main, system_prompt=「依据【片段】回答，引用 [单元id]」, 输入=检索切片) → _turn_end
```

## 三、蜀山验证（测试物，证明方法成立）

- 语料：`tmp/wfr/corpus/shushan/blocks/`（42 块 / ~497KB / 单块 0.7~18KB，30 回）。
- ingest（模型档 ScriptedModel 离线）→ `longdoc.index.json`（42 units + 人物倒排）。
- 3~5 个真实问题（如「李英琼父亲是谁、出场哪些块」「周淳与毛太恩怨在第几块交代」）→ longdoc_ask。
- **量化**：ingest 调用数/读取字节 vs 每次查询读取字节（对比上次全量批① ≈40~50 万 token 开销）——数字说明"结构优先 + 按需"的节省。
- 通用性抽查：用一个**假语料**（几份无关主题 txt）验证机制零蜀山依赖（分块/倒排/检索/回答全通用）。

## 四、边界 / 押后

- 分块策略第一版 = 文件；**章节正则 / 固定窗口已落地（2026-10-26，problems/58）**：ingest 支持
  `@config.longdoc_unit_of = file | chapter | fixed`（+ `longdoc_chunk_chars` / `longdoc_chunk_overlap`），
  索引 units 带 `offset`，retrieve 按字节区间读块；旧索引无 offset → 兼容为整文件。
- **向量臂已落地（2026-10-26，problems/59）**：ComponentRegistry 增 embeddings 槽；ingest 用
  `@config.longdoc_embed` 指名组件 → 每单元 blurb 批量嵌入存 `units[].emb`（meta.embed 记名）；
  retrieve 运行时 hybrid 打分 `@config.longdoc_vector_weight`（缺省 0.5）混合 向量余弦 +
  IDF（归一）；无关键词命中但向量有 → 纯向量（治"第一回讲了什么"这类泛问）；无 embedding
  → 纯关键词（向后兼容）。
- **命中子集升级 merge** —— 押后。
- 骨架提取的模型档**真机质量已验证（2026-10-26，problems/60）**：DeepSeek-flash 真机当骨架器，
  蜀山前 6 块 6/6 出实质 blurb（故事摘要）+ 丰富专名 keys（周淳/李宁/李英琼/毛太/醉道人/白云大师
  等全中）；「妙一」不在前 6 块故缺，符合预期。
- 检索排序 BM25 / 向量 hybrid 复用个人 Wiki 设施可接（向量臂已复用同一 Embeddings 接口）。

## 五、验收

- 单测 `tests/wf/longdoc_test.cpp`（预计 ~8 例）：ingest 双档 / 索引格式 / 倒排 / retrieve 命中排序 / 切片预算截断 / 路径相对 / ask 闭环 / 假语料通用性。
- demo `examples/longdoc`：蜀山 42 块建索引 + 3 问按需答（离线 ScriptedModel）+ 量化打印。
- 验收 problems/57；全仓全绿。