# 29 · wfr-㊾ 批① 验收：mch 三角色 workflow（Planner / Executor / 独立 Reviewer）

> **这份是什么**：wfr-㊾ 批①（arch 21 §四批①）——把单 Agent 的 PEV 升级成**多角色**：
> `mch` workflow = Planner(策划) → 步骤环(串行 Executor + 步内自查 judge) → **Reviewer 独立终审** → final(解说)。
> **不造新原语**：角色 = 节点级 `system_prompt`(独立身份) + 输入裁剪(Reviewer 只读计划+判定)；
> 引擎只给 verify 补了 `review` 相位与 `role` 审计字段。全仓 **681 全绿**(660 基线 + 3 新增 + 既有)。

## 一、改动清单

| 文件 | 内容 |
|---|---|
| `include/cppchain/workflow/ir.hpp` | `NodeIR` 加 `role`(审计角色名)、`criteria`(批级判据,可 `@config.*`) |
| `src/workflow/node_types.cpp` | verify 增加 `review` 相位(`v_verify` 放行 + `c_verify` 分支):批级独立终审;verify parse/write/describe 读显式 `system_prompt`/`role`/`criteria`;judge 落 `plan_log` 行带 `role` |
| `src/workflow/node_types.cpp` | 新 helper `judge_batch_review`(wfr-㊾):只喂 任务目标+步骤与步内判定(plan_log),**不喂执行对话/思考** → Reviewer 输入隔离;输出 `{"pass":bool,"feedback":str}`,不可解析/缺字段 → **fail-closed** |
| `share/wfr/workflows/mch.json` | 内置 `mch` 三角色 workflow:Planner(plan+提议) → loop{announce → Executor(chat_bash+身份) → approve → act → judge(role=executor 自查)} → **review(role=reviewer 独立终审)** → final(解说) |
| `tests/wf/mch_test.cpp` | 3 例:① 角色身份落审计(judge 行 role=executor、review 行 role=reviewer)+ review 输入隔离(不含 📋 执行对话);② review 缺模型 → fail-closed;③ review 返回不可解析 → fail-closed |
| `tests/CMakeLists.txt` | 注册 `mch_test.cpp` |

**实现要点(对照 arch 21 §一/三)**:
- **角色 = 独立身份 + 输入隔离**,不是新技术;Planner/Executor 复用既有 `plan`/`model` 节点的 `system_prompt`;
- **新增极小**:只有 verify 的 `review` 相位(批级终审)与 `role` 字段;
- Reviewer 的「独立」是**身份与窗口**的独立:它只看 目标+步骤+步内判定,**看不到 Executor 的思考与工具往返对话**;
- review 相位**不推进 cursor、不改步骤状态**——批级终审只留判定(plan_log),不动执行指针;
- `role` 字段非空才落 `plan_log`(既有 workflow 不带 role,零破坏)。

## 二、单测(离线,ScriptedModel 驱动)

| 用例 | 断言 |
|---|---|
| `roles_land_in_plan_log_and_review_sees_plan_only` | plan_log 中 judge 2 行 `role=executor`、review 1 行 `role=reviewer`;review 的 human 输入含【步骤与判定】/整批判据,**不含"📋 执行步骤"执行对话** |
| `review_fail_closed_when_model_missing` | review 节点缺 model → verdict=fail、why 含"review 模型未配置" |
| `review_fail_closed_on_unparsable_reply` | review 返回非 JSON → verdict=fail、why 含"未返回可解析" |

## 三、真机对照(同一任务「北京天气 + 穿衣建议」× {pev_l3 单角色, mch 三角色})

同 plan_steps 注入(`s1` 查天气写 `.out/c1_weather.md` + `s2` 读文件给穿衣建议,`s2` 带 `verify:model` 判据);`approve_policy=allow` mock 审批。数据留档 `tmp/mchcmp/`。

| 轮次 | 步骤状态 | plan_log 判定 | 产物(磁盘断言) | final |
|---|---|---|---|---|---|
| pev_l3 r1 | s1 done → s2 failed(cursor1) | judge:`s2 模型裁判不通过(文件 .out/c1_weather.md 不存在、读取失败)` | **无 .out/**(产物缺失) | 口头自述完整穿衣建议(与产物脱节) |
| pev_l3 r2 | s1 done → s2 failed(cursor1) | judge:`s2 模型裁判不通过(未成功读取 .out/c1_weather.md)` | **无 .out/**(产物缺失) | 同上,复现 |
| mch r1 | s1 done → s2 failed(cursor1) | judge fail ×N(端点未解析 JSON,fail-closed) → review **review 未返回可解析 JSON → fail** | `.out/c1_weather.md` **存在(468B)** | 完整结论 + 「产物已生成并校验读取」 |
| **mch r2**(修复提示词的显式 JSON 契约后) | s1 done → s2 **done**(cursor2) | judge 自查 6 行(3 fail → 1 done「引用 11.6~25.9°C」) → **review 批级终审 done**(「满足验收标准」) | `.out/c1_weather.md` + `.out/c2_advice.md` **双落盘** | 完整结论 + 落盘清单 |

## 四、对照结论(诚实)

1. **角色审计贯通(验收判据命中)**:mch 的 `plan_log` 每行都带 `role` —— judge 行 `executor`(执行者自查)、review 行 `reviewer`(独立终审),「任务=角色身份串记入 plan_log」落地。
2. **Reviewer 独立终审若非背书而是约束**:mch r2 里 s2 自查三连 fail,直到重新执行真读了文件、建议引用温度才 done;随即 review 独立复核通过——**两层门都按「步内义务 + 批级要求」工作**。pev_l3 单角色两轮(×2 对称)都是 judge 抓出文件缺失、final 仍输出口头结论(产物双缺)。
3. **端点抖动的诚实性**:mch r1 的 judge/review 全因「未返回可解析 JSON」fail-closed —— 修复 = 身份提示词显式声明 JSON 输出契约(fail-closed 纪律保住了「不做假通过」)。**角色身份 ≠ 免除输出契约**,这是本批学到的注入面纪律延续(23 §)。
4. **产物落盘的差异信号(同任务直接对照,pev_l3 ×2 vs mch r2)**:pev_l3 两轮 `.out/` 均缺失、mch r2 双产物落盘——三角色的 Executor+Reviewer 组合在「真执行」上的压逼,与 problems/24 C5 同型但此处是**同任务两轮对称对照**,信号更硬。
5. **辨析**:`review` 相位(批级独立终审,`kind=review`,不看执行对话)≠ `plan_review` 节点(步骤边界 HITL 确认门,interrupt 悬挂让**人**裁决)。二者互补,不冲突。

## 五、验收

- 全仓 **681/681** 全绿(678 基线 + WfMch 3 例);零告警(clean-first 复核见备注)。
- 内置 `mch` workflow 可被 `wfr list` 识别、`wfr debug --graph-only` 出图(校验+编译通过)。
- 真机数据留档 `tmp/mchcmp/{ws_l3,ws_mch,ws_mch2,ws_l32}/.wfr/sessions/*.log.jsonl` + `.out/*`。
- **遗留与下一步(批②)**:① Send 并行 Executor(按 deps 派发互不依赖步骤)+ 冲突护栏——arch 21 §四批②;② proxyNode 隔离委托仍记下不实现(批②③ 后评估)。

## 备注
- 修正回溯:批① 首版 mch 身份提示词(fixed 但区分「预案」),r2 起显式 JSON 契约 → judge/review 语义判读与 disk 断言双通过。
- 全仓测试数本批从基准补测:README 顶部测试数由 438 → 678 → **681**(2026-10-15 更新)。