# 49 · Project 自管理（第一刀：目录化 + 多任务独立对话 + 项目级注入）验收

> **链路**：[discuss 16](../01-discuss/16-project自管理设计档.md)（🟡 待启动）→ 本设计
> [arch 26](../02-arch-design/26-project自管理设计.md) → 本验收。
> **验收标准（arch 26 §七）**：examples + 单测 8 例全绿；357 workflow 全绿；全仓全绿；demo 三任务
> 独立档案 + 注入可见 + 经验沉淀；wfr 宿主冒烟。自管理 loop 不在本期（押后）。

## 一、落地（改了哪里）

| 件 | 内容 |
|---|---|
| `ir.hpp` | `NodeKind::Project`（新节点类型）；`NodeIR.project`（项目名，可 `@config.*` 或字面量） |
| `node_type.hpp` / `node_types.cpp` | project 节点注册（`v_project`/`c_project`/`e_next`/`t_project`）：begin/end 双相位；`project_dir_path` 新 helper；**`task_archive_path` 双轨**：`@config.project` 非空 → `.wfr/projects/<p>/tasks/<t>.json`（.history 同目录自动跟随），空 → 旧 `.wfr/tasks/`（长程兼容零改动） |
| `validator.cpp` | （无改动：project 经 `t_project` 走既有 VR1/VR4 引用检查） |
| 发布模板 | `share/wfr/workflows/lot_project.json`：lot 的 project 版（load_history → project begin → task begin → 批 loop → final → task end → project end） |

### 关键设计定局（arch 26 对 discuss 16 第一问 6 条的落定）

1. **双轨不迁移**：`.wfr/projects/<p>/` 新体系；旧 `.wfr/tasks/*` 保留（`@config.project` 空即老路径），
   任务列表（project.tasks = 独立任务们）与 task 的 steps（某任务的多步）是两个层次。
2. **任务↔对话一对一 = 宿主约定**：每任务独立 `--session`；项目级上下文 = `project` 节点写入
   `state[system_context]`（与 Context 节点同形状，model 装配时自动拼在系统提示后）——项目上下文
   不进任务会话日志（system_context 不回溯），任务历史不进 project 上下文。**零新拼装机制**。
3. **经验形态**：`lessons.json` 数组 `{when, applicable_if, conclusion, evidence}`；
   `project` end 读 `state.lesson` 约定槽（任务内节点可写）**或宿主 `@config.lesson`**，evidence =
   本批最后 AI 文本；begin 把 lessons 全量注入（相关性挑选留 meta）。
4. **项目级完成标准**：`project.completion_criteria` 进注入 + 复用 `verify review`（本期只接语义）。
5. **触发条件**：入档 `project.triggers`；本期手工触发，`wfr cron` 接法 = 任务名 `project.task` 两段。
6. **自管理 loop 押后**：只圈边界，节点形态留给后续。

## 二、demo 验收（examples/project_run，离线，无模型）

project「monthly_ops」3 任务（weekly / monthly / review），各自独立 session 跑 project→task begin/end 链：

| 断言 | 结果 |
|---|---|
| ① 目录化多文件：每个任务档案落 `.wfr/projects/monthly_ops/tasks/<id>.json` | ✅ 3 个档案独立存在，旧 `.wfr/tasks/` 无副本 |
| ② 多任务 + 独立对话：3 任务 session 互不污染，同一 project 共享 | ✅ 各任务独立复跑、共享 project.json/lessons.json |
| ③ 项目级注入：system_context 含 目标「本月运营综述」/ 纪律「数字一致性」/ 经验 /【本任务】 | ✅ 3 任务全部命中注入断言 |
| 经验沉淀：1 条 → 4 条（每任务 end 追加一条 @config.lesson） | ✅ lessons.size()=4 |
| progress：project.json.progress 三个任务各写入 | ✅ weekly=in_progress monthly=in_progress review=in_progress（plan cursor<steps，确定性） |

### wfr 宿主冒烟（`wfr proj_task --set project=… task=…`）

事件流确认：project begin → task begin（建档 batch1）→ task end → project end，
`system_context` 三块（【项目】目标 / 【项目纪律】/ 【本任务】+ 任务清单）；
最终 `project.json.progress.weekly = in_progress`；任务档案 `weekly.json` 与 `.history/batch1.json`
都在项目目录下。

## 三、单测（tests/wf/project_test.cpp，8 例）

| 编号 | 用例 | 断言 |
|---|---|---|
| 1 | `begin_injects_project_context` | system_context 注入含 目标/完成标准/纪律/经验/【本任务】；state.project 全量快照；results.lessons==1 |
| 2 | `begin_fails_when_project_archive_missing` | project 名无档案 → 运行期终止（Fail-closed），报「缺 project 档案」 |
| 3 | `begin_fails_when_task_not_in_list` | 任务不在 project.tasks → 终止，报「不在 project」 |
| 4 | `task_archive_downsinked_under_project` | @config.project 非空 → 档案 + .history 在项目目录，旧路径无 |
| 5 | `task_archive_legacy_path_without_project` | 无 project → 档案仍走 `.wfr/tasks/`（兼容回归） |
| 6 | `end_writes_lesson_and_progress` | @config.lesson → lessons.json 追加（applicable_if/conclusion/when）；progress[task]=in_progress；results.p_end 携带 |
| 7 | `end_skips_lesson_when_absent` | 无 lesson → 不追加（保持 1 条）、progress 照常、lesson_written=false |
| 8 | `validation_rejects_bad_phase` | phase 非 begin\|end → VN25 |

## 四、回归与全仓

- workflow_tests 349 → **357**（+8）；全仓 **869 全绿**（agent 100 / graph 55 / message 20 / model 59 /
  state 43 / tool 226 / webui 9 / workflow 357）。
- 长程回归：既有 task/lot 测试原样通过（`@config.project` 缺省时路径零变化）。
- wfr 宿主复编后冒烟通过（dev 构建需重编 wfr 才含新节点）。

## 五、遗留（下一刀）

- **自管理 loop**（meta 决定下一任务/经验回流）押后 —— 本期只让结构成立。
- **完成标准自动判定**：project.completion_criteria 目前只进注入 + 语义对 review；「项目级 review 终审
  自动写回 progress」需一个编排案例（demo 未包含）。
- **经验产出通道**：`state.lesson` 是约定槽，任务内如何"模型沉淀经验"的引导节点未做（谁产经验进 demo）。
- 触发条件接 `wfr cron`（任务名 `project.task` 两段）浅改未做。
- lessons 相关性挑选（按 applicable_if 过滤注入）留 meta。