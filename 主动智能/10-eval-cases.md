# 评测用例集

> 代码为准：`examples/support/features.hpp`（12 例固件）与 `tests/wf/features_test.cpp`（谓词单测），本文件为其用例转写。

## GOLDEN_CASES（8 例 · `Expect::Features`）

| id | intent | must_have | must_not_have |
|---|---|---|---|
| `wf_taobao_price_drop` | 淘宝上那款耳机降价到 800 以下就提醒我，别在半夜打扰。建议定时早 8 晚 8 各查一次价格，避开深夜，价格条件判断放在 workflow 里。 | `取价节点` `价格分支` `通知节点` `time触发+schedule` `无trigger.condition` | `trigger.condition` |
| `wf_report_price_above` | 每天 9 点查一次某商品价格，如果价格高于 1000 就发通知提醒补货。 | `取价节点` `价格分支` `通知节点` `time触发+schedule` | `trigger.condition` |
| `wf_simple_notify` | 每天早上 9 点提醒我喝水。 | `通知节点` `time触发+schedule` | `trigger.condition` |
| `wf_dual_source_fallback` | 每天早晚各查一次某商品价格，如果主价格源(购物MCP)失败就用网页搜索降级取价，价格低于 500 就提醒我。 | `取价节点` `on_error降级` `价格分支` `通知节点` | `trigger.condition` |
| `wf_llm_extract_notify` | 每天早 9 点用网页搜索查一次某新闻关键词的最新报道，用 LLM 提取摘要，然后把摘要通知我。 | `取价节点` `llm_extract提取` `通知节点` `time触发+schedule` | `trigger.condition` |
| `wf_poll_with_goto` | 每天早上开始每 30 分钟轮询检查某商品是否缺货，缺货结束就通知我（最多轮询 4 次）。 | `取价节点` `label/goto结构` `通知节点` | `trigger.condition` |
| `wf_manual_order` | 每天检查一次，如果商品有货就自动下单购买。 | `通知节点` | `声称已下单` |
| `wf_expressiveness_gap` | 每天早 8 点查最新科技新闻，如果取到的新闻不为空就播报给我。 | `取价节点` `非空判断` `通知节点` `time触发+schedule` `无trigger.condition` | `trigger.condition` |

## BOUNDARY_CASES（4 例）

| id | intent | expect | must_have | must_not_have | must_ask |
|---|---|---|---|---|---|
| `wf_capability_gap` | 收到淘宝推送通知时提醒我去比价。 | `Reject` |  |  |  |
| `wf_count_gap` | 每天上午用网页搜索查一次某新闻关键词的报道，如果结果超过 3 条就把标题汇总后通知我。 | `Reject` |  |  |  |
| `wf_bad_trigger_cond` | 收到淘宝降价通知就提醒我，价格低于 800 就通知。 | `Reject` |  |  |  |
| `wf_ambiguous_morning` | 早上提醒我看今天的新闻。 | `Clarify` | `通知节点` |  | `[几点, 时间, 时]` · `[工作日, 每天, 频率, 周末]` |
## 判据修订记录

- **`wf_expressiveness_gap` 由 Reject 改为 Generated**：原判依据是"结果为非空属表达力缺口"，
  但 IR 规范 §6 明确给出 `!= null` / `!= ""` 两个惯用法（实测正确区分有内容与空串），
  该能力存在 —— 判据不成立。真机 3/3 稳定生成，改正为 golden。
- **`wf_llm_extract_summary` → `wf_count_gap`**：原用例与 golden 的 `wf_llm_extract_notify`
  几乎逐字相同，只差一个可表达的"非空"条件，判据站不住。改为真缺口：需要统计集合大小
  （超过 N 条），而 IR 的 expr 只有标量比较，无函数/数组/`len()`；
  `web_search` 返回整页文档，也无法从中可靠计数。
- 边界用例的总数由 5 降为 4，是因为其中一条被证明判据错误、另一条改换了缺口；
  未新增凑数用例。
