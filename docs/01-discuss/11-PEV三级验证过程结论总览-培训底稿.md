# 11 · PEV 三级验证专题：过程与结论总览（培训底稿）

> **这份是什么**：wfr-㊹「复杂任务·规划→执行→验证环（PEV）」整个专题的**过程 + 决策 + 可复制方法 + 全部结论**总览，
> 作为**培训资料来源**。每个分篇都有链接，含原始数据留存位置。阅读顺序建议：本页 → [07 建档](07-复杂任务规划执行验证环.md) →
> [08 五框架对照](08-复杂任务处理方法横向对照.md) → [19 架构](..%2F02-arch-design%2F19-复杂任务规划执行验证环设计.md) →
> 24/25/26 验收。**结论可直接引用，方法与流程可照着重跑。**

## 一、这个专题在解决什么（30 秒版）

Agent 做复杂任务时缺的是**任务级组织**：没有"计划"这一等公民、没有每步的完成判据、没有"做一半停下来让人改"。本专题造了一个 **P(规划)→E(执行)→V(验证)** 环，并实现**三级验证强度**、**可中断**、**可回退**。用**真实外部 agent 任务**（不造用例）做了多轮对照，沉淀出哪一级验证在什么场景下有效。

## 二、完整路线图（可当进度表 / 培训流程走）

| 步 | 做了什么 | 产出 / 结论 | 依据 |
|---|---|---|---|
| 1 建档 | 定义 P-E-V + 可中断/可回退的动机与第一问 | 专题档案（待启动） | 07 |
| 2 做法提取 | 五框架对照（deepseek-harness / LangGraph / Codex+SDK / OpenHands / Gemini CLI） | 5 条跨框架共识（计划过确认关 / 无人做确定性产物判据 / 中断三约束 / 回退两层且不做命令反做 / 沙箱护栏底座） | 08 |
| 3 定架构 | 五问五答 + 分期（arch 19） | 计划=state 数据、verify 多态、步骤边界 interrupt、两级回退 | 19 |
| 4 三级实现 | L1（无验证）/ L2（规则判据 probe·rule）/ L3（模型裁判） | 三个 workflow：`pev_l1/l2/l3`；`plan`/`verify` 节点 | 24 附录 |
| 5 中断·回退 | `plan_review` 节点（continue/rollback_to K/abort）+ 引擎 goto 容器名解析 | `pev_review`；真机 continue 两边界、节点级 abort 落审计 | 25 |
| 6 案例批1 | C1–C7（真实用例 × 三级 × 2） | **三级差异规律**（见 §四） | 09 + 24 |
| 7 案例批2 | B2：办公（WorkBuddy Office）/ 出行（DeepSeek V3.2）/ 注入 | 办公三级真过 vs 难案例、注入两道门、出行 DB 面 | 10 + 26 |
| 8 外部工具面 | 引入 `office-docs`（Rust 文档引擎，独立编译、不进主程序）| **工具缺位是真因之一，闭合后小规模三级闭环** | 26 §六之二/三 |

## 三、可复制方法（培训动手部分按此跑）

**三级对照的标准化流程**（任何人可按图施工）：
1. **定义 case**：真实来源（如 WorkBuddy `/tmp/wb_x`、DeepSeek 论文、豆包文档），取任务原文 + 输入 + 预期产出；
   不要自造。每条含：2 步计划（s1 产出、s2 汇报/校验）。
2. **三级步骤**：同一任务，L1 步骤**不写 verify**；L2 在关键步加 `{kind:probe|rule, path/op}`；L3 加 `{kind:model, criteria}`。通过 `--set plan_steps='[...]'` 注入同一 workflow，避免每案一份 workflow。
3. **跑**：`wfr pev_<l1|l2|l3> --workdir <ws> --session <s> --set plan_steps='...' "任务"`（真机需 LLM key；`approve_policy=allow` 可 mock 审批）。
4. **读结论**：会话日志的 `plan_log`（每步 kind/verdict/why）+ `plan.cursor/steps.status` + 产物磁盘断言 + 最终答复。**判据是数据，不信嘴。**
5. **每条 ×2 次**（模型随机性），独立工作区防覆盖。

**开外部工具面（不编译进主程序）的姿势**（office 例）：
- 独立子工程（Rust `office-docs`）→ `cargo build --example demo --release` 独立产出 CLI；
- 包装成 `bin/office <file>`（设 PDFium 环境变量）；
- 在 workflow 的 `system_prompt` 里**明说**该外部命令与用法（模型不会主动发现系统工具）；
- 规模大的任务给足 loop max + 「写脚本批量调用」提示。

## 四、全部结论（可直接引用）

### 三级验证的差异规律（C1–C7 + B2 实证）
1. **L1 = 盲跑保证**：14+ 轮全部 done，哪怕模型自曝"产物没落地"（C4/C7）也标完成。**适合完全可信场景**。
2. **L2 = 确定性产物门（性价比之王）**：缺文件/空/键不对 → 步 fail **不推进 + 重试耗竭**；判据写对时稳稳放行。离线、注入安全。
3. **L3 = 语义/过程门（最严）**：看**执行记录窗口**判"步骤义务"——抓到没给建议/没逐条来源/没步内总结；句法对时背书。**judge 只看步内，final 达成不算步内完成**（刻意）。
4. **产物缺失时三档传感最锋利**：L1 说完成、L2 Absent 截停、L3 读不到+缺结论。
5. **难任务（宽/格式杂）上**：L1 假装完成、L2/L3 反复重判不放过（A1/A3），但预算内可能耗尽 → 与任务规模正相关。

### 工具 / 工程结论
- 外部命令（独立 Rust 引擎）补齐 PDF/DOCX/XLSX 解析后，**同构任务小规模三级全真跑通**（裁剪 A1）。
- `probe_file` **内容签名=整文件等值**，不是子串 → 补 `probe_file_contains`；`value("bytes",0)` ≠ 无约束（坑）。
- `Command.goto_nodes` 不做容器名解析 → 引擎加编译期 `goto_resolve`（`_continue/_break` → 图目标）。
- loop 收口：**all-done 由 announce `goto _break` 兜底**，不再靠 max 空转。
- 多轮真机易踩：**LLM 端点超时**（长任务 900s 被踢，需重跑/裁剪）、**工作区逐轮覆盖丢会话**（独立目录防覆盖）。

### 案例来源（真实，非自造）
- deepseek-harness 文档任务 / DeepSeek-R1 提示词 / DeepSeek-V3.2（出行规划，arXiv:2512.02556）/ deepseek API tool_calls
- 火山方舟·豆包 function-calling / web-search / GUI / apply_patch（官方文档示例）
- WorkBuddy Bench（腾讯，Office/Web/Security/Code，arXiv:2607.20911；本地 `/tmp/wb_x` 已解包）
- "Today" 未确认为可引用源 → 以 MultiWOZ 2.2 + SGD 替补（personal-assistant 类）

## 五、工件 / 数据留存清单
- workflows：`share/wfr/workflows/pev_l1/l2/l3/py/review/trip.json`
- 外部引擎：`office-docs/`（独立 Rust 子工程）
- 数据：`tmp/pevcmp_b/`（B2 各 ws + logs；A/C 对照会话）、`tmp/pevcmp/wfr`?（第一批 `tmp/pevcmp/`）
- 结论文档：`04-problems/24`（C1–C7）/ `25`（批④）/ `26`（B2 + 裁剪闭环）

## 六、后续建议（培训后的方向池）
- 裁剪 A1/A3 全量重跑 + V3 全量×2 收口数据完整度；长任务（05 #3）专题承接"超时/时长预算"议题；
- `strategy.on_fail` 自动回退（验证失败自动回第 K 步）——批④ 已具备 rollback 原语；
- 把「office 外部命令」接入姿势沉淀成 12 步教程条目。