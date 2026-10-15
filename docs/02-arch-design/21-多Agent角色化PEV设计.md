# 21 · 多 Agent·角色化 PEV 设计（wfr-㊾ 定架构 · 从设计档 15 的 A 起步）

> **前文**：设计档 [15](../01-discuss/15-多Agent协调设计档.md)（四方向）→ 选 A：把单 Agent 的 PEV
> （plan/execute/review/final）升级成**多角色**。核心主张：**角色 = 独立身份（system_prompt）+
> 输入隔离（只看该角色该看的东西）**；不改引擎原语，主要做“给现有节点配角色身份 + 组装一个三角色 workflow”。
> 从 A 起步：先落地「Planner / Executor / **独立 Reviewer**」，并行 Executor（Send）作为二期。

## 一、从现有件到角色（“不造新原语”）

| PEV 单角色（已实现） | 多角色化 | 改动量 |
|---|---|---|
| `plan` 节点（模型产计划） | **Planner 角色**：独立身份 prompt（“任务策划：产出步骤+判据”），上下文=目标+历史摘要 | 极小（一个 system_prompt） |
| loop 的 `think`（执行一步） | **Executor 角色**：每步身份（“执行者，只做指定动作并落盘”） | 结论：model 节点 system（已有） |
| `verify(judge)` | **Reviewer 角色**：独立身份 + **输入隔离**（只喂 步骤目标+执行记录文本+判据，不喂 Executor 的思考）—— 现有 `judge_model_review` 已是“看记录+判据”的隔离形态，新增=显式「你是独立审查者，勿盲从执行者自述」身份 | 半小：verify 节点已有独立 `model`+`system_prompt`，补身份提示（默认文案已有） |
| `final` 节点 | **解说者**：整合各角色产出给最终答案 | Visit：无 |

**关键洞察**：PEV 的 `verify(model)` 已经是“**当角色**（裁判）执行”，它看的是**执行记录窗口**而不是全局会话——
这正是角色上下文隔离的雏形。角色化的装置 = **身份 prompt + 输入裁剪的显式声明**；真正的新增只是一个
「列为角色」的显式 workflow 与一个「独立 Reviewer 不盲从执行者」的身份提示。

## 二、你要的“角色化”落地形态（mch workflow）

```
mch = Planner(plan) → 步骤环（串行 Executor ×N，每步可带 verify）→ Reviewer 独立终审 → final(解说)
```
- **Planner**：产 `state.plan`（goal+steps+verify 判据）——同 `plan` 节点，身份为「任务策划者」。
- **Executor**：`loop` 的 `think`（model 节点）——身份 prompt 固定为「执行者：只管本项目，落盘产物」（chat_bash）。
- **Reviewer**：在每步 `judge` 之外再设一个**可选终审**：`verify {kind:model, criteria:整批要求}` 作为独立裁判，
  身份提示显式「独立审查员：基于执行记录与判据独立判断，不信任执行者的口头自述」。
  - **与既有 `judge` 的分工**：`judge`（步内轻判）vs `Reviewer`（批级复核）——相当于“执行者自查 + 外部平行审查”两层。
- **final**：解说（汇总各角色视角）。

## 三、第一问收敛（设计档 → 定论）
1. **角色落地粒度**：**图节点复用（不加新节点类型）**——角色 = 节点级 `system_prompt` + 输入裁剪；需要时
   加一个 `role` 字段便于审计（记录“此步由哪角色写/读”）。
2. **共享记忆切法**：会话日志 = 全局（都能看），但**输入窗口裁剪**是角色区分的机制（Reviewer 不看 Executor 思考，只看记录+判据）。
3. **并行度**：一期**串行** Executor；二期 `Send` 并行派发互不依赖步骤（D 的收口）。
4. **验证可信**：一期先对照「单 Agent同任务 vs Planner+Executor+Reviewer」，看 Reviewer 独立是否提升可信/可查；
   产物仍用 verify rule/probe 客观探。
5. **放哪**：A 起步（本稿）。

## 四、分期
| 批 | 内容 | 验收 |
|---|---|---|
| ① | `mch` 串行三角色 workflow + Reviewer 独立身份；对照 `pev_l3` 单角色差异 | 单测（角色身份串/审记入 plan_log）+ 真机一轮（同一任务两份对照） |
| ② | 并行 Executor（M10 `Send` 按 deps 派发互不依赖步骤）+ 冲突护栏 | examples + 单测 |
| ③ | 交接式（A→B handoff 交接件契约）接入 | problems/NN 验收 |

> **预留原语 · proxyNode（隔离委托，先记下不实现）**：一个节点指定**要代理的 workflow 路径**，
> 并指定**运行方式**（如：隔离会话 = 独立上下文跑、只回传产物；独立线程 = 并发执行）——
> 这是"M8 子代理配置化"与"隔离委托"的将来载体。**本期不做**（import 只覆盖"角色复用"这一档；
> 隔离/并行运行是另一档原语），定位置：批②/批③ 之后评估再启用。核心待定项：路径解析（按 wfr_lookup）、
> 运行方式枚举、产物回传契约（文件/结构化）。

## 五、边界
- 不做：agent 间自由对话/自协调循环；不做“多模型厂商混跑”（同一 `main`）。
- Reviewer 的「独立」是**身份与窗口**的独立，不是模型不同——真“另一模型/另一厂商”留给将来（kersp: 那才叫真独立，先用窗口独立验证）。

## 待办锚点
- 关联：设计档 15 · PEV(arch19/11) · M8(arch09) · 五框架(08)。
- **✅ 批① 已落地（2026-10-15）**：`mch` workflow + verify `review` 相位 + `role` 审计 + 真机 4 轮对照，见 problems/29。
- 下一步：**批②**（Send 并行 Executor 按 deps 派发互不依赖步骤 + 冲突护栏）→ 批③（交接式）。