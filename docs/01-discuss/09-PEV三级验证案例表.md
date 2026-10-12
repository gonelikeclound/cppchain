# 09 · PEV 三级验证对比案例表（待审批稿 · 2026-10-12）

> **这份是什么**：wfr-㊹ 三级验证（L1 无验证 / L2 规则 / L3 LLM 裁判）的**对比任务清单**。
> 全部案例取自真实公开 agent 测试/文档，**非自造**；每行标来源 URL。三级对照目标：
> 同一任务在 `pev_l1` / `pev_l2` / `pev_l3` 各跑一轮，观测「步骤状态 / plan_log / 最终答复 /
> 验证判定」差异（L1 盲跑会不会漏、L2 确定性判据抓不抓得住、L3 模型裁判质量把关）。
> **状态：待你审批** —— 批了才动跑对照。
>
> 图例：`可跑`=当前 chat 工具集直接能跑；`环境替代`=原任务工具（SendMessage/apply_patch/schedule…）
> 以 file 系工具等价替换（验收语义等价、并如实标注）；`待工具面`=需加工具/pythons/GUI/dialog 等，暂不首跑。

## 一、推荐首跑（7 条，可跑/环境替代）

| # | 来源（真实用例） | 任务原文（节选） | 工具 | 三级观测点 |
|---|---|---|---|---|
| C1 | 豆包·function-calling（[URL](https://www.volcengine.com/docs/ark/doc/function-calling)） | 「北京今天的天气如何？适合穿什么衣服？」 | `get_weather` | L1：查完即过，穿衣建议可能与数据脱节；L2：步骤产物（天气+建议）写盘→`rule(json_key)` 校验数据在；L3：裁判判「穿衣建议与天气数据一致性」 |
| C2 | 豆包·function-calling 分支发消息（[URL](https://www.volcengine.com/docs/ark/doc/function-calling)） | 「先查询北京的天气，如果是晴天微信发给Alan，否则发给Peter」 | `get_weather`+`file_write`（**环境替代**：原 task 是 SendMessage，这里以写 `.out/notify.txt` 记录「收件人=Alan/Peter」等价） | L1：分支可能瞎写；L2：探针查 notify.txt 存在且内容仅 Alan/Peter 之一；L3：裁判判「分支依据与天气数据一致」 |
| C3 | deepseek-harness / DeepSeek-R1（[URL1](https://api-docs.deepseek.com/guides/tool_calls) [URL2](https://github.com/deepseek-ai/DeepSeek-R1/blob/main/README.md)）+ 豆包 web-search（[URL](https://www.volcengine.com/docs/ark/doc/web-search)） | 「今天有什么 AI 领域的热点新闻？列出 3 条并带来源」 | `web_search`+`file_write` | L1：可能纯凭记忆；L2：`.out/hot.json` 需含 `sources` 数组→`rule(json_key)`；L3：裁判判「有来源引用、覆盖多源」 |
| C4 | deepseek-harness 指南（[URL](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/user/guide/index.md)） | 「Summarize this repository and identify its main packages.」 | `file_read`/`list_dir`+`file_write` | L1：总结可能无主包名；L2：`.out/repo_summary.md` 含仓库关键词→`probe(contains)`；L3：裁判判「主页包识别有据」 |
| C5 | 豆包·apply-patch（[URL](https://www.volcengine.com/docs/ark/doc/apply-patch)） | 「Use apply_patch… change the output of the last print statement in demo.py to “world”（只改最后一行，其余两行不动）」 | `file_write`（**环境替代**：原 task 是 apply_patch，这里用 file_write 重写文件等价） | L1：可能整文件乱写；L2：`.out/demo.py` 含 `print("world")`→`probe(contains)`；L3：裁判判「只改了最后一行、其余行未动」 |
| C6 | deepseek-harness 定时（[URL](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/user/guide/schedule.md)） | 「Remind me every day at 23:00 in Asia/Shanghai to check the weather.」 | `file_write`（**环境替代**：原 task 是 schedule_create，这里写 `.out/schedule.json`） | L1：可能漏写时区/写成 Once；L2：`rule(json_key)` 需 `cron=daily` + `tz=Asia/Shanghai`；L3：裁判判「规则完整、语义正确」 |
| C7 | workbuddy Office·api-usage（[URL](https://github.com/Tencent/workbuddy-bench)） | 只读探索一个示例源码仓库，产出「CLI/Python API 怎么用」的 Markdown 说明，关键说法给路径证据，不改任何文件 | `file_read`/`list_dir`+`file_write` | L1：说法可能无据；L2：`.out/api_usage.md` 非空+含关键词→`rule(file_nonempty)`；L3：裁判判「claim-citation 绑定、可操作性」 |

## 二、备选池（真实用例已建档，按需扩工具后启用）

| 来源 | 用例清单 | 为什么未首跑 |
|---|---|---|
| WorkBuddy·Office（[URL](https://github.com/Tencent/workbuddy-bench)） | contract-extract / stock-fund-return-compare / invoice-email-archive-manifest / analyst-forecast-extract / subscription-credit-reconcile / cross-week-dashboard-migration | 需 **python/office 库执行**（pandas/openpyxl/bash）；当前 pev 链无代码执行工具 → 待工具面 |
| WorkBuddy·Web/Security/Code | browser-clipper-extension / chart-generation / blind-ssrf-redis / schema_drift | Web 需浏览器&前端产物验收、Security 需挑战 docker、Code 需仓库级 python → 待工具面 |
| deepseek·V3.2（[URL](https://arxiv.org/html/2512.02556)） | 三日出行规划（12 城市/酒店/餐饮/交通 DB 工具 + submit_result 验收） | 需**业务 DB 工具面**（城市/酒店/餐厅…+验证器）→ 待工具面，且其「搜索难、验证易」恰是三级差异的好试验，值得扩 |
| 豆包·GUI（[URL](https://www.volcengine.com/docs/ark/doc/gui-task-processing)） | GUI 多步 + click grounding | 需 **GUI agent 工具面**（截图/点击坐标）+ 屏幕环境 → 待工具面 |
| MultiWOZ 2.2 / SGD（[URL1](https://github.com/budzianowski/multiwoz) [URL2](https://github.com/google-research-datasets/dstc8-schema-guided-dialogue)）（「Today」未确认为可引用个人助理测试源，以此替补） | 酒店/餐厅/火车 booked 槽位跟踪类 goal | 需 **dialog/task API 工具面**（book/search/multi-turn），当前 pev 为文件型工具 → 待工具面 |

## 三、三级差异的观测口径（对每条 C1–C7 同法）

对同一任务分别跑 `pev_l1` / `pev_l2` / `pev_l3`，记录并对照：
1. **步骤状态 & cursor**：L2 的 fail（探针/规则不中）会不会出现、L1 是否"盲推进"到底
2. **plan_log 验证判定**：L1=none 全 done / L2=probe·rule 判据行 / L3=模型裁判包 feedback
3. **最终答复质量**：引用了文件产物吗、和产物一致吗、有来源吗
4. **错误率/返工**：L2/L3 是否在某步触发 retry/回退，L1 是否会带病前行
> 每条各跑 2 次（可重复），以回应模型随机性；逐条出对照结论。

## 待你审批的点

- [ ] 首跑集 **C1–C7** 是否放行（含 C2/C5/C6 的「环境替代」声明）
- [ ] 备选池里要不要**先扩哪个工具面**（建议下一位：code-exec 面跑 WorkBuddy Office 三条，或「出行规划」业务 DB 面）
- [ ] 每条跑 2 次的随机性控制 / 预算（真机 LLM 调用条数）