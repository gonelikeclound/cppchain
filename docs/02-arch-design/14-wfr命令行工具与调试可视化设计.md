# 14 · wfr 命令行工具与调试可视化（wfr 系列设计）

> wfr 系列落地记录（wfr-①~⑤ 全部实现，写实文档）。
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
- 执行顺序：`read → load 动态库（顶层）→ expand_workflow_imports → parse → compile`。

## 4. 调试可视化（`wfr debug`）

数据管线（`debug_view.{hpp,cpp}`，纯 C++ 导出 + 单 HTML）：

```
IR ──debug_view_graph──▶ {nodes, edges}       边=NodeType::edges()，goto 回边、终端圆点
日志 ──debug_view_logs──▶ 按 run 分组 + C++ 重放 loop_counts（merge_slots）→ 每条记录内嵌 loop
      build_debug_payload（>20k 裁节点诊断）→ render_debug_html → 自包含单 HTML
```

- **HTML 结构**（纯 JS/SVG，零网络依赖）：入口 BFS 分层布局；节点矩形可点；
  左栏 run 下拉 + 节点列表 + **循环分组 tab**（goto 1..max 迭代）；中栏图；右栏日志
  （engine enter/exit/error/interrupt + 节点诊断，含 run/node/loop/update）。
- **双向联动**：点节点 → 过滤右栏日志 + 高亮；点日志 → 自动切 run + 高亮节点 + 滚动。
- 防注入：`script_data_escape`（`<`→`<` 等）；`html.ends_with("</html>")` 等测试锁。
- 调试台展示**展开后**的扁平图（import 节点带前缀显形：demo 里 `notify_wf__n`）。

## 5. 验收

- **全仓 438+ 测试全绿、clean-first 零告警**；新增项（wfr 相关单测）：
  - ① `.wfr` 迁移路径断言；② 无 trigger parse/编译+fire；③ import 展开 8 项；⑤ debug_view 4 项。
- 手工端到端（`build/dev/bin/wfr`）：
  - `wfr list` → 内置 chat + `loop_wf / notify_wf`（trigger=manual）；
  - `wfr chat --session c1 "你好"` → 真机回复；`"我叫小明记住"` → 二次续聊记住名字
    （load_history 历史续接，多轮聊天成立）；
  - `wfr loop_wf --session s1 "检查"`（在 `examples/wfr_demo`，`item_id` 内置 P1）→ 退出 0、
    会话日志落 `.wfr/sessions/`、日志可见 `loop_counts{gp:1..3}`（goto `max:3` 循环）；
  - `wfr debug loop_wf --all` → `.wfr/debug/loop_wf.html`（图含 import 节点
    `notify_wf__n`、gp 1..3 分组素材）。

## 6. 开放点

1. `$_` 跨 import 边界语义放宽（位置概念可能越界到子链/父链）——文档化，v1 接受。
2. 跨 workflow 的 label 续接由作者负责（用 `_turn_end` 显式终止），行为测试固化。
3. `wfr debug` 布局对 50+ 节点图质量下降（第一版够用）；超大日志裁节点诊断保结构。
4. 真定时调度（cron 常驻）不做；`checkpoints/` 预留（续跑走 `FireOptions.fork`）。
5. HTML 每节点只显示简要文本与 update 截断；完整 diff 查看留作体验增强。