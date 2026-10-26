# 59 · longdoc 向量臂：blurb 语义检索（复用 Embeddings 设施）

> **链路**：arch 28（problems/57，骨架+倒排+按需深读）→ 分块策略（problems/58）→ 本档 = **向量臂**。
> 目标：泛问"第一回讲了什么""这部书大概讲什么"这类**无专名**问题——关键词倒排（keys 是专名/主题）
> 命中不了，要靠 **blurb 的语义相似度**兜住。复用仓库同一 `model::Embeddings` 设施（个人 Wiki /
> 工具检索同款），不为 longdoc 再造一套。

## 一、问题

arch 28 检索 = query 分词 → keys 倒排 IDF。keys 是**专名/主题**（李英琼、周淳、毛太…），所以
"李英琼父亲是谁"这类**点名**问题命中极准；但"第一回讲了什么""整体讲了什么"这类**泛问**分词后
（第一/回/讲/什么…）在 keys 里一个都命中不了 → 空命中、答不了。缺一条"按语义找相近 blurb"的路。

## 二、方案

### 1. 组件注册表
`ComponentRegistry` 增 `embeddings` 槽（`name → model::Embeddings*`）+ `embedding(name)` 访问器——
与 `models` 同构，宿主注入真实 DashScope / 测试注入 fake。

### 2. ingest（建索引时出向量）
- `@config.longdoc_embed` 指名 embedding 组件（缺省空 = 不出向量，索引无 emb）。
- 每个单元 **blurb** 批量 `embed()` 一次（≤ max_batch_size 由实现自动分批）→ `units[].emb`；
  `meta.embed` 记组件名（retrieve 据此找组件，不用重复指名）。
- 嵌入失败 → 索引无 emb + warn 日志，退纯关键词（fail-open）。

### 3. retrieve（hybrid 打分）
- 关键词 IDF 分归一（÷ 最大分 → [0,1]）；
- 向量分 = `cosine_similarity(query_emb, unit.emb)`（夹 0）——query 每次 embed_query；
- 混合：`final = vector_weight*vec + (1-vector_weight)*norm_kw`；
  - 无关键词命中但向量有 → **纯向量**（泛问的救生索）；
  - 无 embedding / 索引无 emb → **纯关键词**（向后兼容，旧索引照常）；
- `@config.longdoc_vector_weight`（缺省 0.5，0 即纯关键词）。
- hits 增 `score`（最终分）与结果 `vector`（本次是否走了向量）可追踪。

## 三、实现（src/workflow/node_types.cpp longdoc 段 + components.hpp）

- `components.hpp`：embeddings 槽 + 访问器（include embeddings.hpp）。
- `longdoc_double_param`（[0,1] 小数 config 解析）、`longdoc_emb_from_json`（JSON 数组→Embedding）。
- ingest 捕获 components；retrieve 捕获 components；向量路径全部运行时解析（组件名来自 config/索引）。

## 四、测试（tests/wf/longdoc_test.cpp 新增 3 例）

| 测试 | 断言 |
|---|---|
| `ingest_vector_embeds_unit_blurbs` | embed 组件 → 每 units[].emb（256 维）、meta.embed 记名 |
| `retrieve_vector_resolves_abstract_query` | 泛问"女儿"：keys 空命中（中文规则档）→ **纯向量**命中 a.txt（其 blurb 含"女儿"），vector=true |
| `retrieve_vector_disabled_without_embedder` | 无组件 → 索引无 emb、meta.embed 空；泛问零命中 + vector=false（纯关键词兼容） |

假 embedding = 256 维字符袋（字节计数）；共享字符 → 余弦高，够测语义命中。既有 12 例（含分块策略）
原样通过（无 embed 路径不动）。

## 五、结果

- 全仓 **923 全绿**（920 + 3）；longdoc 测试 12→15。
- arch 28 §四 更新；向后兼容：旧索引/无 embedding 全退纯关键词。

## 六、遗留 / 下一刀

- **命中子集升级 merge**（需要全局整合时把命中单元合并深读）—— 押后。
- 骨架提取模型档真机质量（blurb/keys 准不准；向量臂的语义质量也依赖 blurb）—— 真机试。
- 检索排序 BM25 / 向量权重自适应 —— 押后（当前固定 vector_weight）。