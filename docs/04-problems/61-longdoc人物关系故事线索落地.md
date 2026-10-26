# 61 · longdoc 人物关系/故事线索：抽取聚合 + 作答节点

> **链路**：arch 28（机制 57 → 分块 58 → 向量臂 59 → 骨架真机质量 60）→ 本档补**全局综合**：
> 人物关系梳理 / 主要故事线索。真机实证先行（problems/60 后追加的探针），本档把机制正式化为节点。

## 一、问题

现有 longdoc 是**单元级**事实检索（倒排/向量），答「李英琼父亲是谁」这类点名问极好；但
「主角的人物关系」「主要故事线索」是**跨单元全局综合**——要先把散在各块的专名/事件织成关系网与
线索链。缺这一层，这两类问题答不好。

## 二、方案（在 ingest 内加抽取维度，不新增建索引的调用）

### 1. ingest 模型档提示词（一次调用，成本不涨）
骨架提示词追加请求 **relations**（`[{a,b,rel}]`，a/b 人物名、rel 类型如 父女/师徒/故交/仇怨/同门/主仆）
与 **thread**（一句话主线进展）→ 单次调用同时出 blurb/keys/relations/thread。
规则档无模型 → relations/thread 缺省（关系节点 fail-closed 提示模型档重建）。

### 2. 索引 v4（预聚合）
- units[]：模型档时带 `relations` + `thread`；
- 顶层 **`rel_net`**：跨块去重聚合的关系网（a-b **排序键**去重 + **引用块集合**；展示保留模型
  原始 a/b 序）+ **`threads`**（{id, thread} 线索列表）。

### 3. 节点 `longdoc_relation`
- 输入：索引 + `@config.longdoc_rel_query`；`@config.longdoc_rel_limit`（关系供给上限，缺省 100）。
- 输出：把 **【用户问题】+【人物关系网】（带 [块名] 引用）+【故事线索】** 写进 system_context
  （同 retrieve：节点不自己调模型，由 workflow 里的 model 节点作答）。
- 索引无 rel_net/threads → **fail-closed**（"索引无人物关系/线索数据（需模型档重建）"）。
- 模板 `longdoc_relask.json`（relation → model → _turn_end）。

## 三、测试（tests/wf/longdoc_test.cpp 新增 3 例）

| 测试 | 断言 |
|---|---|
| `ingest_extracts_relations_and_thread` | 模型档 relations/thread 进 units；rel_net 按 a-b 排序键**去重**（李宁\|周淳 一条、两块引用）、展示保留原 a/b 序；threads 列表 |
| `relation_node_builds_context_from_net` | 关系节点 → system_context 含 问题+【人物关系网】「李宁 — 周淳：故交 [a.txt b.txt]」+【故事线索】；results.relations=4 / threads=3 |
| `relation_node_missing_data_fails_closed` | 规则档索引无 rel_net → 节点报「索引无人物关系/线索数据」 |

## 四、真机实证（探针 tmp/longdoc_rel.cpp，DeepSeek-flash）

6 块抽 → 去重 **39 条关系网**（周淳—李宁 故交 / 李宁—英琼 父女 / 周淳—毛太 仇怨 / 白云大师—蛇妖 敌对 /
餐霞大师—蜈蚣 主仆 / 毛太—邓明 师徒 … 全带引用块）：
- 「李宁与周淳是什么关系？」→ **故交**（引 [周淳—李宁]）
- 「李英琼与父亲、周淳的关系怎么发展？」→ 父女 + **先世交叔侄、后转师徒**，诚实标边界（老头子=李宁? 未明示，不编）
- 「梳理主要人物关系网」→ 以周淳为中心的系统梳理（家庭/故旧/师徒/结义/仇怨线/蛇妖线）
- 「主线故事线索」→ **双线交织、汇向「赴成都杀毛太」**，逐块引用

## 五、结果

- 全仓 **926 全绿**（923 + 3）；longdoc 测试 15→18。
- arch 28 §四 更新；模板 `longdoc_relask.json` 入 share。

## 六、遗留 / 说明

- **命中子集升级 merge**（通用"全局整合"：关键词/向量命中的子集升级为合并深读）—— 押后。
- 关系/线索抽取质量同样依赖模型档（规则档无）；真机实测良好（见 problems/60 的骨架质量 + 本档实证）。
- 检索排序 BM25 / 向量权重自适应 —— 押后。