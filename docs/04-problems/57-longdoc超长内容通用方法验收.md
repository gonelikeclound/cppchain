# 57 · longdoc：应对超长内容的通用方法验收（arch 28）

> **链路**：蜀山超长上下文痛点（全量 Map/merge ≈40~50 万 token 太贵）→ 用户定调"方法为目标、蜀山为测试物"
> → [arch 28](../02-arch-design/28-longdoc超长内容通用方法设计.md) → 本验收。
> **验收标准（arch 28 §五）**：longdoc 三件（ingest/retrieve/ask 模板 + longdoc.index.json）与语料无关；
> 8 单测；离线 demo 用蜀山 42 块量化按需加载 vs 全量；全仓全绿。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `ir.hpp` | `NodeKind::LongdocIngest / LongdocRetrieve`；`NodeIR.longdoc_root/index/query/topk/slice_chars` |
| `node_types.cpp` | longdoc 三 helper（`longdoc_tokenize` 空白+标点分词、`longdoc_key_match` 双向子串、`longdoc_int_param`）+ 两节点：**ingest** 单遍扫描目录（每文件=单元，排序稳定）→ 每单元骨架 {blurb, keys}（模型档/规则档）+ keys 倒排 → 落 `longdoc.index.json`；**retrieve** query 分词 → keys 倒排 IDF 加权 top-k → 读命中单元原文切片（预算截断）→ **写 system_context（【用户问题】+ 检索片段，model 自动带上）** + results.hits |
| `validator.cpp` | VR2 白名单加 longdoc 两节点 |
| 模板 | `share/wfr/workflows/longdoc_ask.json`：retrieve → model（带引用要求） |
| 测试 | `tests/wf/longdoc_test.cpp` 8 例 |
| demo | `examples/longdoc`：蜀山 42 块建索引 + 3 问按需答 + 量化 |

### 关键语义（方法 vs 载体）

1. **与语料无关**：索引格式抽象为 `units[]`（id/标题/骨架/键/路径/字节）+ `keys` 倒排；分块策略第一版=文件
   （小说按块、代码按文件、文档按节皆可）；骨架提取提示词通用（"给这段一句话 + 专名/主题"）。
2. **三层范式**：结构优先（单遍扫 → 轻量骨架）→ 倒排（键→单元）→ 按需深读（只读命中切片 + 引用）。
3. **双档骨架**：模型档（每单元一次轻量调用）；规则档（blurb=首行、ASCII 词表 keys，中文语料 keys 空
   —— fail-open 不阻断建索引，倒排留模型档）。
4. **按需加载进 model 上下文**：retrieve 把【用户问题】+ 检索片段写 system_context，model 节点零新拼接
   自动带上；无需 load_history 也能答（查询不落会话历史）。

## 二、单测（tests/wf/longdoc_test.cpp，8 例）

| 用例 | 断言 |
|---|---|
| `ingest_rule_builds_index` | 无模型 → blurb=首行、ASCII 块 keys=词表、中文块 keys 空、倒排正确、索引落盘 |
| `ingest_model_extracts_skeleton` | 模型产 blurb+keys → 排序去重、倒排 键→[单元下标] |
| `retrieve_topk_and_slice` | query 命中倒排 → top-k、切片加载、system_context 注入【单元id】 |
| `retrieve_respects_topk` | 多单元命中同分 → topk 收敛取先 |
| `retrieve_slice_budget_truncates` | 切片 ≤ 预算 + “…” 标注；bytes=全文 |
| `retrieve_no_hit_note` | 无命中 → hits 空 + note |
| `ask_closes_loop_with_citation` | 检索切片进 model 上下文 + 答复带 [单元id] |
| `shushan_rule_ingest_42_units` | 蜀山 42 块规则 ingest → 42 单元、blurb 非空（语料缺失时 SKIP） |

## 三、demo 验收（examples/longdoc，离线，蜀山 42 块 ~497KB）

| 断言 | 结果 |
|---|---|
| ① 建索引：42 单元（497467 字节）落 longdoc.index.json | ✅ |
| ② 「李英琼的父亲是谁？」→ 命中 block_001/002（读入 24901 字节）| ✅ |
| ② 「周淳与谁结怨？」→ 命中 block_001/002 | ✅ |
| ② 「妙通是谁？」→ 命中 block_002/007 | ✅ |
| ③ 答复带引用 [block_001_1.txt] | ✅（骨架器/回答器为离线脚本化模拟，真机换真实 LLM）|
| **量化：三问共读 68792 字节 ≈ 全量 13.83%** | ✅ 按需加载的节省成立 |

## 四、回归与全仓

- workflow_tests 395 → **403**（+8）；全仓 **915 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 403，零失败零错误）。
- 既有 wfr/project/longdoc 系列全部原样通过（新节点独立注册，无既有路径改动）。

## 五、遗留 / 下一刀（arch 28 §四）

- 分块策略进阶（章节正则/固定窗口）；**向量臂**（复用个人 Wiki 向量，泛问"第一回讲了什么"这类无专名
  问题靠 blurb 语义检索）；命中子集升级 merge（深路径）。
- 骨架提取的**真机质量**（blurb/keys 准不准）——离线已验证机制，真机待跑。
- 检索排序 = 关键词命中 + IDF；BM25/hybrid 可接（个人 Wiki 设施）。

> 附：蜀山只是验证载体——这套 longdoc 机制对任意超长文本（文档/代码/日志/书）通用；本次未做蜀山
> 专属索引，只做了机制 + 用蜀山数据证明它可行。