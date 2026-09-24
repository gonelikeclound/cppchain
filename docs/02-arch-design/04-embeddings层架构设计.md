# 04 · Embeddings 层架构设计（向量能力）

> 不属于原路线图的 M0–M10 主线，是为「大批量工具查找」卡位问题（04-problems/01）补的**能力层**：
> 没有它，向量检索这一解法无法验证。落地后同时可供后续 RAG / 记忆检索复用。

---

## 1. 定位与边界

- **与 ChatModel 并列**的另一类模型能力：`Embeddings` 抽象。
- **依赖**：`cppchain_model`（ModelError / Transport 注入）。
- **提供**：批量嵌入（自动分批、顺序保证）、单一适配器（DashScope）、余弦相似度工具、以及工具层的 `EmbeddingSelector`。
- **不做**：向量数据库/索引结构（HNSW 等）、本地模型推理、重排（rerank）模型 —— 需要时再引入。

## 2. 实测协议（DashScope 原生，非 OpenAI 格式）

```
POST {base}/api/v1/services/embeddings/text-embedding/text-embedding
  → {"model":"qwen3.7-text-embedding","input":{"texts":[…]}}
  ← {"output":{"embeddings":[{"text_index":0,"embedding":[1024 维]}]},
     "usage":{"total_tokens":N},"request_id":"…"}
  错误 ← {"code":"InvalidApiKey|InvalidParameter|…","message":"…","request_id":"…"}
```

四条实测约束（都直接影响了实现）：

| 约束 | 实现对策 |
|---|---|
| 请求体是 `input.texts`（嵌套对象） | 独立适配器，不复用 OpenAI wire |
| 返回**不保证有序**（靠 `text_index`） | 解析后按 `text_index` 重排；乱序/缺失 → Parse 错误 |
| **批量上限 20**（服务端硬限制） | `max_batch_size`（默认 20，可调小）+ 自动分批 |
| 错误体带 `code` | 错误映射同时看 HTTP 状态与 `code`（`InvalidApiKey`→Auth） |

## 3. 接口

```cpp
using Embedding = std::vector<float>;

struct EmbeddingConfig {
  std::string api_key, base_url, model;
  long timeout_ms = 60000;
  std::size_t max_batch_size = 20;
  std::string ca_info;              // 企业代理环境
};

class Embeddings {
 public:
  virtual std::expected<std::vector<Embedding>, ModelError> embed(
      std::span<std::string const> texts) = 0;   // 顺序与输入严格一致；自动分批
  virtual std::size_t dimension() const = 0;
  virtual std::size_t max_batch_size() const = 0;
  std::expected<Embedding, ModelError> embed_query(std::string_view);  // 便捷
};

double cosine_similarity(Embedding const&, Embedding const&);

// 工具层：向量检索（含关键词降级）
class EmbeddingSelector final : public ToolSelector {
  std::expected<void, ModelError> build_index(ToolRegistry const&);  // 批量嵌入工具文本
  std::vector<ToolSpec const*> select(reg, query, k) const override; // 嵌入查询 + 余弦 top-k
  std::optional<ModelError> last_error() const;                      // 已降级时置位
};
```

## 4. 关键决策

| 决策 | 结论 | 理由 |
|---|---|---|
| 同步 vs 异步 | **同步门面**（信号量桥接异步传输） | 嵌入是"批量一次拿全"的语义，无流式需求；与 `ChatModel::invoke` 同一模式 |
| 索引在哪 | **`EmbeddingSelector` 内**（vector<Embedding> 与注册表同序） | N 小（千级）时线性扫描足够；索引结构留到需要时 |
| 查询失败 | **降级到关键词**（可关） | 检索不可用会让整个 agent 失效；降级比失败更符合工程直觉 |
| 查询文本 | `工具名 + 可检索描述（描述+参数名+参数描述）` | 复用关键词检索的同一份文本定义，保证**对照组可比** |

## 5. 验证结论

见 `04-problems/01` §6：**跨语言/同义改写场景下 关键词 2/8 → 向量 8/8**（top-1 领域命中），
代价是每查询多一次网络往返（3.57 ms → 126 ms）与一次性索引成本（200 条 / 6.5 s / 7770 tokens）。

**评测局限已在文档中如实标注**：合成工具集的"动作"差异不可区分，故指标只到**领域级**。

## 6. 待办 / 开放点

1. **混合召回**：关键词 ∪ 向量 → 去重 →（可选）LLM 精排；需 A/B 量化收益。
2. **动作级评测集**：用可区分动作的真实工具集重测"A 选对工具"的能力。
3. **向量索引**：N 上万时线性点积与内存都不划算，需 HNSW/量化或外部向量库。
4. **缓存**：工具索引可持久化（与 M5 的 Store 有交集）；查询嵌入可缓存。
5. 多 provider：当前只有 DashScope 原生格式；OpenAI 兼容的 `/embeddings` 可另加适配器。