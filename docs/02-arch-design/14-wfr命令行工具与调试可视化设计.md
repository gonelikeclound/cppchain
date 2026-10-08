# 14 · wfr 命令行工具与调试可视化（wfr 系列设计）

> wfr 系列落地记录（wfr-①~㉕ 全部实现，写实文档）。
> 定位：给 workflow 一个**发行级入口**（按名执行）与**可调试性**（单 HTML 调试台）。

## 1. 定位与边界

- **提供**：`src/wfr`（正式 CLI，构建产物 `build/dev/bin/wfr`）命令行运行器；
  workflow 增强（trigger 可选 / import 复用）；
  单 HTML 调试台（`wfr debug`）。
- **不做**：OS 级调度（trigger 仅元数据，D1 语义）；真并行；cron 常驻；HTTP server。
- 数据约定：**统一 `.wfr/` meta 目录**（`include/cppchain/workflow/wfr.hpp`）：
  `workflows/<name>.json`（按名查找）、`sessions/`（会话日志，`session_path` 已迁）、
  `checkpoints/`（预留）、`debug/`（生成物）、`.gitignore`（忽略运行态、保留源）。

## 2. 命令行 `wfr`

- `wfr <name-or-path> [--workdir] [--session] [--set k=v]... [--reset] [--allow-import] <query>...`
  - `<name>` → `.wfr/workflows/<name>.json`；含 `/` 或 `*.json` 且存在 → 当文件。
  - query → `input_messages`（`__input__` 事件进日志）；`--set` 注入 `@config.*` 键。
  - runner 不做 config_keys 白名单（白名单是生成器侧防模型发明键的；wfr 跑手写 workflow）。
  - trigger 省略即 `manual（wfr 拉起）`；含 model 节点才需要 LLM key，纯 tool 离线跑。
- 子命令：`wfr list` / `wfr debug <name> [--session N|--all] [--out F] [--open]`。
- 退出码：0 成功 / 2 用法・找不到・解析校验编译失败 / 1 fire 运行时错误。
- 骨架复用：`examples/workflow_cli`（argv 手写、读 IR→load imports→expand→parse→compile→fire）。

### 2.1 内置/固定 workflow 与查找约定（发布结构）

- **发布自带目录** `share/wfr/workflows/*.json`（源码随仓库维护，安装分发到
  `<prefix>/share/wfr/workflows`；开发构建经编译宏 `WFR_SHARE_WORKFLOWS_DIR` 指向源码目录，
  `WFR_WORKFLOWS_DIR` 环境可覆盖此兜底层）。内置 `chat`（load_history + model 的最简单聊天，
  多轮续聊）即以文件归档于此，可看可改。
- **查找顺序**（名字命中即停）：
  显式路径 > `<ws>/.wfr/settings.json` 的 `"workflow_dirs"`（数组、相对 ws 解析、顺序即优先级）>
  `<ws>/.wfr/workflows` > `~/.wfr/settings.json` 的 `"workflow_dirs"`（相对 ~ 解析）>
  `~/.wfr/workflows`（全局用户，发布后每用户共享）> 发布自带。
  **workspace 的 `.wfr` 路径本身不可被覆盖** —— 两个 settings 都只能**追加** `workflow_dirs`。
- 实现：`workflow/wfr_lookup.{hpp,cpp}`（`wfr_read_settings` / `wfr_lookup_dirs` /
  `wfr_lookup_workflow` / `wfr_list_workflows_in`，8 项单测：顺序 / 相对路径 / 坏 settings
  fail-open / 找不到）。CLI 的 `list` 按来源目录列出。
- 正式工具目录 = tool 库内建 `make_datetime_tool` + `make_calculator_tool`（不依赖 demo fakes）。

## 3. workflow 增强（本系列在 workflow 库的改动）

### 3.1 trigger 可选
- `WorkflowIR::trigger` 升 `std::optional<TriggerIR>`：parse 允许缺失（给出须 object）、
  `to_json` 省略键；validator VT 段在无 trigger 时短路（VT5 空 cron 不再拦）。
  编译/fire 全程不读 trigger（D1）。生成端两提示词注明"可省略 = 宿主手工触发"。
- 读数点守卫：proactive / wf_eval / features.hpp。

### 3.2 import 其他 workflow（展开式）
- 新 `import.{hpp,cpp}`：顶层键 `import`（单数，避开动态库 `imports` 数组）。
- 算法：递归读被引 IR → **前缀化**其全部节点 id 与指针（next/then/else/goto/default/
  fallback/cases 值）及 `$<id>` 引用（scan_refs 识别）→ 拼入扁平 nodes → 引擎零改动。
  父层可用子链**原始 id** 引用（自动映射到前缀最终 id）。
- fail-closed：循环 import（路径链）拒、找不到报 searched 路径、重名拒、被引 wf 的
  动态库导入拒（v1）；被引 trigger 丢弃。loader 可注入（测试内存）。
- **传参 `with`**（wfr-㉕）：`"with": { key: <字面量> | "@config.<父层键>" }`，把子链节点里
  **整串恰为** `@config.<key>` 的字符串叶，在展开期折叠成字面量（常数化）或转写为父层
  运行时配置引用；未绑定键原样保留。同一子 workflow 可被 import 多次、各带不同参数
  实例化（靠 `as` 前缀区分命名空间）。
- **子链徽标 `origin_wf`**（wfr-㉔）：被拉入节点的 `origin_wf` = 其**直属**子工作流名；
  `as` 只改前缀不改归属；嵌套时仅标尚无徽标者（A→B→notify 的节点归 `notify_wf`，不归 B）
  → 调试台/日志可按"子链"归组（见 §3.3）。
- 执行顺序：`read → load 动态库（顶层）→ expand_workflow_imports（含 with 折叠 / origin_wf
  打标）→ parse → compile`。

### 3.3 组合分层（原语 → 参数化 import → 运行时组合）

- 组合单元自小到大：
  1. **原语节点**——`model` / `route` / `run_tools` / `expr` / `tool` / `label` / `goto` /
     `load_history` / `echo`（极小可组配单元；`wf.json` 把"聊天+工具"拆成 agent 循环即证；
     `echo` 为回显节点：`text` 模板（`$节点.result.字段` / `@config.*`）解析后打印到终端
     并落一条节点诊断，不改 state；model/run_tools 发布可引用 result 供 echo 引用）；
  2. **workflow**——JSON 节点清单，`import` 把子 workflow **展开**进父（`as` 命名空间 +
     `with` 参数化 + `origin_wf` 徽标）；
  3. **运行时组合**——`Send`（M10 动态 fan-out 到子图）、agent 层 handoff/workers、
     组件注册表（`model`/`toolset` 由宿主命名注入，`main`/`all` 即实例）。
- **现状边界**：import 是**展开式**——编译期扁平成单 IR，子链无运行时子图边界/独立状态/
  错误域，与父链同一命名空间（VN2 全图唯一）。
- 待定缺口见 §6（显式入参接口、`with` 仅覆盖字符串叶、运行时子图边界）。

## 4. 调试可视化（`wfr debug`）

数据管线（`debug_view.{hpp,cpp}`，纯 C++ 导出 + 单 HTML）：

```
IR ──debug_view_graph──▶ {nodes, edges}       边=NodeType::edges()，goto 回边、终端圆点
日志 ──debug_view_logs──▶ 按 run 分组 + C++ 重放 loop_counts（merge_slots）→ 每条记录内嵌 loop
      build_debug_payload（>20k 裁节点诊断）→ render_debug_html → 自包含单 HTML
```

- **HTML 结构**（纯 JS/SVG，零网络依赖）：三栏可拖布局（左导航 | 中：图 + 节点 JSON 详情 |
  右：树形日志）；分栏 pointer 拖拽 + localStorage 持久化；图里**内置节点**（`__input__` 置顶、
  `_turn_end`/`_fail` 置底，虚线灰边）也一并绘出；图例在节点区底部。
- **树形日志**（轮次→workflow→节点→记录）：run 去重键 = `会话名/run`（wfr-㉓ 起跑前用
  `SessionLog::max_run_seq` 垫引擎计数 → 跨进程调用轮次不重号）；workflow 归属 =
  节点 `origin_wf`（子链）→ 记录 `workflow` 徽标（父）→ 按节点在页内/「（其它）」推断；
  节点行单行省略号截断；**选中日志**在右栏下半部 `#logdetail` 显示该条**美化 JSON**
  （缩进 + key/字符串/数字/布尔语法高亮），再点取消。
- **双向联动**：点图节点/导航 → 该节点记录全量日志中标 `.hit` + 图下方出节点完整 JSON 定义
  （`def` = `NodeIR.raw`）+ 自动展开滚到分组；点日志行 → 对应节点被选中高亮 + `.sel`。
- 防注入：`script_data_escape`（`<`→`\u003c` 等）；`html.ends_with("</html>")` 等测试锁。
- 调试台展示**展开后**的扁平图（import 节点带前缀显形：demo 里 `notify_wf__n`）。

## 5. 验收

- **全仓 449 测试全绿、clean-first 零告警**；新增项（wfr 相关单测）：
  - ① `.wfr` 迁移路径断言；② 无 trigger parse/编译+fire；③ import 展开 9 项
    （含 with 传参、origin_wf 徽标、嵌套保留最深归属）；⑤ debug_view 4 项。
- 手工端到端（`build/dev/bin/wfr`）：
  - `wfr list` → 内置 chat + `loop_wf / notify_wf`（trigger=manual）；
  - `wfr chat --session c1 "你好"` → 真机回复；`"我叫小明记住"` → 二次续聊记住名字
    （load_history 历史续接，多轮聊天成立）；
  - `wfr loop_wf --session s1 "检查"`（在 `examples/wfr_demo`，`item_id` 内置 P1）→ 退出 0、
    会话日志落 `.wfr/sessions/`、日志可见 `loop_counts{gp:1..3}`（goto `max:3` 循环）；
  - `wfr debug loop_wf --all` → `.wfr/debug/loop_wf.html`（图含 import 节点
    `notify_wf__n`、gp 1..3 分组素材）；
  - `wfr debug chat --open` → 新版三栏调试台（内置节点、轮次/workflow 树、
    选中日志美化 JSON）；纯 tool 的离线 `paramcombo`（import 子链 + `with` 传参）
    能跑通并能在调试台看到带 `origin_wf` 的子链分支。

## 5.5 chat2：把历史形态做成可对比的自变量（wfr-㉙）

`share/wfr/workflows/chat2.json` 是内置 `chat` 的**超集**（多一个 `echo_hist` 节点，
把历史渲染统计打在终端上），`chat.json` 原样不动 —— 修改过程与新旧对比本身可回看。
细节与三档定义见 [15](15-思考模型推理回传与历史压缩策略设计.md)。

三档全部走 `@config.*`，所以**不用编辑 JSON** 就能切：

```bash
wfr chat2 --session s "北京天气怎么样？"                                  # 缺省 = 现有 chat 行为
wfr chat2 --session s --set history_mode=drop_round     "你刚才说多少度？"
wfr chat2 --session s --set history_mode=keep_evidence --set evidence_budget=50 "…"
wfr chat2 --session s --set reasoning_echo=always --set reasoning_effort=high "…"
```

A/B 的正确姿势是**同一段历史 × 同一句话 × 不同渲染**：把同一份 `.wfr/sessions/session-X.log.jsonl`
复制三份，分别用三档问同一句话，再 `wfr debug chat2 --all --open` 三份并排
（点 `recall` 节点看渲染统计 JSON）。

## 6. 开放点

1. `$_` 跨 import 边界语义放宽（位置概念可能越界到子链/父链）——文档化，v1 接受。
2. 跨 workflow 的 label 续接由作者负责（用 `_turn_end` 显式终止），行为测试固化。
3. `wfr debug` 布局对 50+ 节点图质量下降（第一版够用）；超大日志裁节点诊断保结构。
4. 真定时调度（cron 常驻）不做；`checkpoints/` 预留（续跑走 `FireOptions.fork`）。
5. HTML 每节点只显示简要文本与 update 截断；完整 diff 查看留作体验增强。
6. import 是**展开式**：子链无运行时**子图边界**（独立状态/错误域/局部 checkpoint）——
   "可组合单元"的边界属性未实现（候选：运行时子图节点，或 import 加实例上下文）。
7. `with` 传参只覆盖**整串恰为** `@config.<key>` 的字符串叶（不折叠嵌在较长字符串中的
   引用、不改数组/对象结构）；子链不声明参数契约，无入参/参数类型校验。
8. 一个子 workflow 被 import 多实例靠 `as` 前缀区分命名空间，但运行时无**实例上下文**
   （每实例的入参绑定校验、独立 run/日志域）——调试台目前的"子链归组"靠 `origin_wf` 徽标。