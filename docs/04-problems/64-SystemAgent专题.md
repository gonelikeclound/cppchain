# 64 · SystemAgent 专题（开题）

> 2026-11-02 开题。本篇是**专题骨架**：先立编号与结构，把「要研究什么」留给逐轮讨论逐步回填。
> 讨论结论一律回写到本文件对应小节，避免散落在会话里——按项目惯例，最终要有可运行 demo 验证。
>
> **《SystemAgent》语义首轮待定**（§1）。先明确的是：它是一个**研究专题**，不是一次性修复——
> 现有检查 ± 后续按「研究 → 设计 → 落地 → 验收」推进，关联的既有资产先盘在 §3 备用，范围定了再裁剪。

---

## 一、开题结论（✅ 2026-11-03 · 第 1~2 轮已定）

> 第 1 轮给出研究源并指认整理任务、第 2 轮拍板 cppchain 落地形态后，本节省去占位、转正为结论。
> 研究源为 `docs/01-discuss/system-agent/`（01–07 + `_research/01–05`）与 `system-agent-doubao` 设计档。

- **定义（已定）**：SA =「**事件驱动 + 状态机驱动**」的**系统级 Agent**，与宿主 DSH 现有 **ReAct 循环式
  Agent** 对照。形状 = `事件 → 状态跃迁 →（仅在声明的接缝处咨询模型）→ 新状态 → … → 终态`；
  核心差异不是"循环 vs 事件"，而是 **谁站在每次跃迁的关键路径上**——LLM 从"每次跃迁必经"降级为
  "显式 gate 上被咨询的专家"，确定性跃迁交机器推进（=`01`/`02` 一句话模型）。
- **空白原语（调研判断）**：显式迁移表 × 可记账 LLM 咨询 × 咨询等待期进程可死——业界普遍三选二；
  v1 倾向先做 B（状态机）+D（咨询）最小闭环。
- **目标（承接设计主张）**：带记账咨询的 statechart 运行时；SA 只编排不执行、动作只作用于图（I1 可验收）。
- **组织形态（Round 2 定，用户表述）**：**SA 不是单个 agent，而是一组 agent**——构建一个
  **SA 核心 agent** + 几个**不同领域的 agent**，各完成不同的工作。核心 agent 负责分诊/编排/跨域协调，
  领域 agent 各自持有域内能力与状态机；整体落在 cppchain 既有多智能体原语上（agent loop /
  subagent / Send / handoff / 子 workflow）。
- **落地载体（Round 2 定）**：demos 单独目录 + mock 独立 so ——
  - 为 SA 提供**专门的 demo 目录**（拟 `examples/sa_demo/`，按仓库 demo 惯例一个可执行目标 + README）；
  - **所有给 SA 专门 mock 的代码放进独立的 `samock` 共享库**（拟 `tests/plugin_demo/samock.cpp` →
    `libsamock.so`，沿用 `sidefx_notify.so` 的先例：`add_library(... SHARED)` + 外部目录 env
    `WFR_TOOL_LIB_DIRS`/`CPPCHAIN_SAMOCK_DIR` 加载，纯 C descriptor + 函数指针、不链 cppchain）。
- **落点（Round 2 定）**：基于 **cppchain** 落地（非 DSH）——DSH 侧零件
  （`ctx.jobs`/`send`/`schedule`/testkit 四件）由 cppchain 既有的 graph/workflow 引擎、agent 循环、
  多智能体原语、副作用治理（journal/审批/审计）承接，缺口收敛为 **Graph Runtime 的 SA 语义层
  （事件漏斗 / 两层上下文 / 咨询 gate / Reaper 善后）** 与 **samock 测试外围**。

## 二、逐轮讨论记录（倒序，最新在前）

### 第 13 轮（2026-11-03 · S8：任务历史保留/删除 + 下载 + 更多示例）

- **用户需求**：① SA 页面保留过去历史，用户可删除，删除即持久删除；② 增加下载等其他 demo。
- **历史库**：`web/sa/sa_tasklog.json` 从「每次覆盖的结果文件」升级为**历史库**：
  - 合并规则：同 id 覆盖（脚本 demo 的 t1/t2 稳定 id，不会越跑越多）；一句话规划任务用
    `p<epoch-ms>` 唯一 id，每次运行**追加**；新任务在前（最近优先），上限 60 条；
  - 原子写（`.tmp` → `rename`），页面不会读到半截 JSON；
  - 脚本模式与规划模式都走同一合并函数（`load_history`/`merge_history`/`write_json_atomic`）。
- **删除（持久）**：`sa_cgi?action=delete&id=<task id>` —— id 白名单字符（≤64），从历史库移除并原子落盘；
  不存在则 `ok=false` 明确报错；页面每条任务卡右上角 `×`，删除后重拉历史（若删的是当前任务则自动选第一条）；
  运行中禁止删除（UI 侧拦截）。
- **并发护栏**：`sa_cgi` 运行路径加 `flock(web/sa/.run.lock)` 单运行锁——两个标签页同时点运行时，
  后到者明确返回「已有一次运行在进行」，避免 tasklog/进度互相覆盖（实测并发验证）。
- **下载（纯前端 Blob，无新增服务端端点）**：左栏「操作」行——「⬇ 下载当前」（当前任务
  dag+messages+facts JSON）、「⬇ 下载历史」（全部 tasks 的 sa-tasklog.json）、「删除当前」；
  任务卡上的删除从不易发现的 `×` 改为文字按钮「删除」（同一持久删除端点）。
- **更多 demo**：左栏示例 chip ×7 —— 电话+改口 / 只发微信 / 只打电话 / 电话→微信 / 微信→电话 /
  发给爸爸 / 约饭+改口，**一键运行**（填词并直接进入「LLM 规划 → DAG → 执行」，结果进入历史）；
  运行中重复点击被 UI 拦截（服务端 flock 兜底）。7 条均已用真机模型逐一跑通（单域 / 双域顺序 /
  改口改道三类形态）。
- **回执修正**：历史库多条任务时，CGI 回执的 `title/task_status/dag_*` 只取**本次新任务**
  （数组最近优先取首条；脚本模式只回传 t1/t2），修复「新任务已执行但状态条显示旧标题」。
- **验收**：E2E 扩至 **16/16**——离线 fixture 连跑两次断言历史保留>1 条；删除后服务端文件真实减少且
  重复删除报错；页面骨架新增「生成 DAG 并运行 / 下载历史」标记；全仓串行 **1092/1092** 不变。
- 备注：历史合并后，生成文件不再与 `sample-tasklog.json` 逐字节等价（第 12 轮的说法只适用于空的
  全新数据目录）；`sample` 继续作为页面静态回退数据源。

### 第 12 轮（2026-11-03 · S7：一句话 → DAG（真机 LLM Planner）→ 校验 → 执行）

- **用户澄清关键点**：SA 的核心不是「运行 DAG」，而是「**生成 DAG**」——能否根据用户一句话产出
  一张 DAG，然后把它跑起来。选定范围：**直接接真机 LLM Planner**。
- **分层**（LLM 只出现在规划 gate；域执行保持确定性、可回放）：
  1. **Planner**：`sa_demo --query="…"` → OpenAI 兼容端点（.env：`LLM_*`，本项目 deepseek-flash）
     产出**受限 IR**（role ∈ user/core/phone/msg/world；edge kind ∈ query/dispatch/report/control/world）；
     解析容忍 ```json 围栏与前后解释文字；不合法 → 把 issue 回喂模型重试（≤3 次），仍不过则 fail-closed 拒跑。
  2. **静态校验**（`examples/sa_demo/sa_plan.hpp`，纯函数）：P00–P29 —— 角色/kind 白名单、
     id 唯一、恰好 1 user/1 world、域节点必须有 core→域 dispatch、core 必须有 dispatch 出边、
     无环、所有节点在 user→world 路径上、control 只能 user→phone 且必须配 msg 域、规模上限（节点 3..12）。
  3. **通用解释器**：按拓扑序执行——user 消息 → core 分诊 dispatch → 电话域拨号/拒接重拨/接通
     （有 control 边则 barge_in 改道）→ 消息域 send_message → world 节点逐项查世界断言；
     消息与 tasklog/进度事件双写，UI 实时可视化；断言不过则任务 Failed。
  4. **入口**：CLI `--query` / `--plan-json`（跳过模型，离线可测）/ `--dry-run`；
     CGI `?planner=llm&query=<一句话>`（URL 解码在 CGI 做，query 经 **env `SA_QUERY`** 传子进程，
     不拼 shell，零注入面）；页面左栏新增「一句话」输入框 + 「生成 DAG 并运行」。
- **实测（真机 deepseek-flash）**：
  - 「给妈妈打电话…拒接再打一次，改口就发微信」→ 6 节点/6 边（含 control 边）→ 执行
    Completed、16 消息、7 里程碑、钟 27s；
  - 「给妈妈发微信说我晚上不回家吃饭」→ 4 节点/3 边 → 5 消息、钟 0s；
  - 电话-only（无 control）→ `c1 state=talking` 断言通过、6 里程碑。
- **测试纪律（R5）**：真机 Planner 不进 CI——新增 `sa_plan_tests`（12 条纯函数用例：合法计划、
  缺 world、环、未知角色、域缺 dispatch、core 无 dispatch、control 指向非电话、改口无 msg、
  phone-only 合法、布局、JSON 抽取）；E2E 扩至 **14/14**：离线 fixture 走完整校验+解释器
  （16 消息/7 里程碑/钟 27s）+ 坏计划（缺 world）必须拒跑且零产出；全仓串行 **1092/1092**。
- **诚实边界**：本轮的「智能」在规划 gate；域内工具序仍由确定性执行器（电话 FSM/消息发送）驱动，
  不是让 LLM 自由调工具。下一步可把域节点也交给 agent loop（真机），并让 Reaper/事件漏斗接管失败恢复。

### 第 11 轮（2026-11-03 · S6c：运行可视化——后端真慢跑 + 页面实时进度）

- **用户反馈**：「电话 mock 的例子太快了，看不到执行过程，mock 时候慢一点」。
  选定方案 = **后端真慢跑 + 实时进度**（非前端回放动画）。
- **sa_demo**：新增 `--progress=<JSONL>` 与 `--pace-ms=<0..5000>`：
  - 边跑边 append 结构化事件（逐行 flush）：`run start/done` · `task start/done` ·
    `dag`（节点×边先给页面）· `node`（active/done）· `msg`（与 tasklog 消息同口径）；
  - 每条 `msg` 后 sleep `pace-ms`（默认 0 = 全速，直跑/库测试不受影响）；
  - 事件内容与最终 tasklog **逐字对齐**（工具行由 args/输出统一成文；`clock_forward` 记在推进前）；
  - t1 DAG/t2 DAG 提到导演段前定义，实时进度与 S4 tasklog **共用同一份**（防漂移）。
- **sa_cgi**：`?scenario=all&pace=300` —— pace 只接受纯数字（≤5000，防 shell 注入），
  先 truncate `sa_progress.jsonl` 再 spawn（避免页面读到上一次残留）；回执新增 `pace_ms`/`progress`。
- **页面**：运行期间轮询 `sa_progress.jsonl`（200ms），节点 active 脉冲/已跑 done/未跑 pending，
  消息逐条追加，状态条显示「实时执行 · N ms/步」；只跟随本次运行的首个任务（t1 电话例）；
  回执到达后停止轮询并以 tasklog 重新装载（进度只是观测面，tasklog 仍是唯一真相）。
- **验收**：E2E 扩至 **12/12**——B 阶段 `pace=80` 并断言进度事件齐全（run/task/dag/node/msg、
  t1 `n_call` active、make_call/assert 消息）；C 阶段 `e2e=run&pace=80` 全通；
  生成 tasklog 与 `sample-tasklog.json` **逐字节等价**（diff 为空）。
- 体验：点「▶ 运行 demo」后能看见 用户 → 核心分诊 → 电话域拨号/拒接/重拨/接通 → 改口 →
  核心改道 → 消息域送达 → 世界断言 的完整过程（默认 300ms/步，约 6 秒）。

### 第 10 轮（2026-11-03 · S6b 修正：图源码 = DAG 定义，非仓库 C++ 源码）

- **用户纠正**：「源码」标签要的是 **SA 生成的 DAG 图的源码**（该图节点×边的定义本身），
  第 9 轮误做成「读仓库 C++ 源码」。
- **修正**：源码标签改为展示**当前任务 `dag` 定义** —— 「清单视图」（node `id [role] layer·row — label — detail`
  逐行 + edge `from --(label,kind)--> to`）⇄ 「原始 JSON」（`JSON.stringify(dag, null, 2)`）双视角；
  数据 = 页内 sa-tasklog 的 `cur.dag`，**纯前端、零外部读取**。
- **移除** `sa_cgi` 的 `?action=src` 端点（封闭名单读仓库文件那套整体删除，不留死代码/攻击面）；
  `sa_cgi` 回到纯「触发运行 + 回执 + run_meta」。
- **E2E 回归 11/11**：B 阶段 src 三检删除；C 阶段 `e2e=show` 扩展——切到「源码」标签并断言
  **图源码已渲染**（dagsrc 并入 render 判定，实测 ok）。
- 体验：右上面板 [节点图] / [源码] 互为「同一张图的两面」——渲染视图 vs 定义视图。

### 第 9 轮（2026-11-03 · S6b 右上「源码」标签页）

- **用户反馈**：「demo 的关系没看懂」→ 给右上面板加**标签页：`节点图 / 源码`**，源码页直读仓库源文件对照
  —— DAG 的每条边（分诊/改口/上报/写世界）都能在 `main.cpp` 导演段与 `samock.cpp` 假电话剧本里找到出处。
- **`sa_cgi` 新增只读端点 `?action=src&file=<key>`**（text/plain）：**封闭允许名单**六键
  main/samock/guard/cgi/e2e/page（用户输入只命中名单键、路径零拼接 → 局域网可达服务也不暴露越权读取）；
  **零副作用**（不 spawn、不动 run_meta）；run 路径向后兼容（`action` 缺省即 run）。
- **页面**：右上面板头变标签页（节点图 / 源码）；源码页 = 文件下拉 + `<pre>` 只读视图；
  非 WebUI 环境优雅降级提示。
- **实测**：`src&file=main` 200 含锚点 · `find=boom` 名单外拒 · **E2E 扩至 14/14**（新增 src 三检：
  内容锚点 / 零副作用（run_meta 不变）/ 名单外拒绝；A 阶段骨架标记加「源码」）。
- **实证排查两件（结论=无 bug）**：① `run_meta` 快照/还原逻辑实测 work（99→0 正确还原）；
  ② headless Chrome `e2e=run` 经隔离 `SA_DATA_DIR` 实测**只触发 1 次** run（非连发）。
- **run_meta 去噪**：历轮 E2E/手动触发在 `web/sa/run_meta.json`（gitignored 宿主观测）累积出不可追溯的
  18:xx 噪音簇，已重置为干净基线（total=0，页面「第 N 次」从 1 重新计数；原拟保留的 S5 早期 3 条亦
  被清理条件误伤一并清掉，如实备注——无业务价值损失）。

### 第 8 轮（2026-11-03 · S6 WebUI E2E 测试）

- **S6 全过（11/11）**：`run-sa-e2e.sh` —— SA 编排台 WebUI 端到端，驱动**真实服务**
  `python3 -m http.server --cgi`（同 run-webui.sh），**三阶段**：
  - **A 静态 HTTP**：`/sa/` 渲染骨架（SA 编排台 / 任务列表 / 运行 demo / DAG 四标记）+ 
    `sample-tasklog.json` 契约（schema=sa-tasklog/v1 · tasks=[t1,t2]）；
  - **B CGI 运行整路**：`scenario=all` 真实触发 sa_cgi→sa_demo —— JSON 回执（ok · elapsed_ms≥0 ·
    task_ids 含 t1/t2）、磁盘三产物（tasklog 2 任务 / run_meta total≥1 / run.log）、
    **run.log 含「S1 + S2 + S3 + S4 全部通过」=执行真发生**、确定性数值贯通
    （facts.milestones==7 · clock_end_sec==27 · core==2）、**第二次 run_total 递增**（append 语义）、
    `scenario=t1` → v1 明确拒答（含 v2 提示）；
  - **C 浏览器 DOM**：headless Chrome（`--dump-dom` × 页面内建 `?e2e=show/run` 钩子）——
    `show`：任务列表 + DAG 真实渲染（`data-e2e-render=ok`）；`run`：**真实点击「▶ 运行 demo」
    → fetch CGI →「运行完成」回执**（`data-e2e-run=pass`）。
- **隔离方案**：跑真实 `web/sa/`（保证页面↔CGI 数据同源），测试前后**快照/还原**三个运行态文件
  （sa_tasklog.json / run_meta.json / run.log），`trap EXIT` 清理服务器 / Chrome / 快照目录；
  已实测还原干净（run_meta 回 3、`.e2e_snapshot` 已删）。
- **同轮配套**：`sa_cgi` 加 **`SA_DATA_DIR`** env 旋钮（数据目录缺省 `<root>/web/sa`，可指临时目录做隔离）。
- **页面小修**（C 阶段暴露的瞬态竞态）：`runCgi` 成功/失败**直落 `data-e2e-run` 钩子**、
  `loadMeta()` 不再覆盖刚完成的「✓ 运行完成」、15s 看门狗兜底。
- **诚实边界**：E2E 覆盖 HTTP/CGI/磁盘效果 + headless Chrome 渲染/点击闭合；**非** playwright 人工
  视觉校验（环境只有 chrome --dump-dom，无 playwright），真手指视觉留给人工抽检。
- 下一步：**S7+** —— 事件漏斗 / L1 任务表 / Reaper 善后；**CGI v2**（单场景触发）并行候选。
- **脚本定位**：入口 `run-sa-e2e.sh`（仓库根，`./run-sa-e2e.sh [--no-build] [--no-chrome]`）；
  通过/失败计数打印、退出码=失败条数。

### 第 7 轮（2026-11-03 · S5 CGI 运行端点）

- **S5 全过**：浏览器 `GET /cgi-bin/sa_cgi?scenario=all` → `application/json` 运行回执；页面「▶ 运行 demo」
  按钮触发 → 重拉 tasklog/meta、保留当前选中。
- **`sa_cgi`（examples 目标，薄壳零 cppchain 依赖）**：解析 `QUERY_STRING`（scenario；
  **v1 仅 all** —— t1/t2 单场景触发 = v2，需把 S2/S4 导演段抽成可单独调用，已按契约明确拒答）
  → 定位 sa_demo 二进制（`/proc/self/exe` 真实路径同目录，或 `SA_DEMO_BIN` env 覆盖）
  → `chdir` 仓库根 → `popen` 子进程（stdout/stderr → `web/sa/run.log`）→ 测墙钟
  → 追加 `web/sa/run_meta.json`（runs 保留最近 50 条，gitignored 宿主观测、不进 tasklog）
  → 输出 JSON（ok / elapsed_ms / run_total / last_run_at / task_ids / tasklog 绝对路径）。
- **sa_demo 参数化**：`--tasklog=<path>`（默认 `web/sa/sa_tasklog.json`；CGI 传绝对路径），余下行为不变。
- **页面**：左栏加「▶ 运行 demo」+ `#run-status` 三态（运行中 / ✓耗时·次数·相对时间 / ✗失败原因）；
  `loadData(keepSel)` 重拉并保留当前选中；`loadMeta()` 显示 CGI 累计（上次 ms · 相对时间）；
  非 WebUI 环境（fetch /cgi-bin 404）优雅降级提示。
- **实测**：手动 CGI env 模拟 ✓（11ms · [t1,t2] · total 递增）；`python3 -m http.server --cgi` 全通路
  ✓（scenario=all → ok；scenario=t1 → 按 v1 契约明确拒答；/sa/、tasklog、run_meta 全 200）；
  run_meta 每次成功调用真实追加。
- **诚实边界**：sa_cgi 只做「触发 + 回执 + meta」，执行语义全在 sa_demo（全离线确定性，
  R5 纪律不碰真模型）。
- 下一步：**S6** —— 事件漏斗 / L1 任务表 / Reaper 善后；**v2** 单场景触发（抽导演段）并行候选。

### 第 6 轮（2026-11-03 · S4 SA 专用 UI 页落地）

- **S4 全过（17ms）**：SA 专用 UI 页 [`web/sa/index.html`](../../web/sa/index.html)（自包含单文件）。
- **页面布局（按用户规格原样）**：**左右两栏** —— 左 = **任务列表**（query + SA 回传消息 + 用户补充信息，
  组成一个完整任务）；右 = **任务详情**，分上下两部分 —— **上 = DAG 节点图，下 = 对话/消息列表，
  各占 50%、分隔条可拖动**（外纵/内横两条分隔，窄屏自动改轴）。
- **数据契约 `sa-tasklog/v1`**：`tasks[] = {query, user_supplements, sa_replies, facts,
  dag{nodes[layer×row/role/detail/state], edges[kind]}, messages[kind/node], milestones_summary}`。
- **demo S4 段（两条真实任务）**：
  - **t1 C7 简版**：6 节点 DAG —— 用户→核心①→电话域→核心②→消息域→世界（含`改口(barge_in)`控制平面边）；
    16 条消息 + 7 里程碑总结；核心 2 咨询（S2 断言已锁）；
  - **t2 消息直路**：单域快路（= 分诊退化结论，05 §2）—— 核心 1 咨询，4 节点链，无里程碑；
  - 事实（钟/里程碑/世界）**实时读 samock** 拼装，唤醒账用 S2/S4 断言锁定的常量；写
    `web/sa/sa_tasklog.json`（gitignore），页面回退内置 `sample-tasklog.json` + 拖放/导入 JSON。
- **页面实现要点**：主题 token 化（深色=套件基调，配浅色变体）；DAG = 内联 SVG
  （layer×row 网格布局、角色配色 currentColor、边 kind 着色 query/dispatch/report/control/world、
  marker 箭头 + 边标签）；**点节点 → 检查条显示执行细节 + 下方消息按节点筛选**；消息 7 种 kind 徽章
  （查询/SA/分诊/工具/里程碑/控制/断言）+ 虚拟钟 `t=Ns`。**零 CGI**：run-webui.sh 静态服务即看
  （`http://localhost:8000/sa/`）；「从页面触发运行」需 CGI run 端点，留作后续。
- 下一步：**S5+** —— 事件漏斗 / L1 任务表 / Reaper 善后；或给页面加「运行触发」。

### 第 5 轮（2026-11-03 · S3 M0 化落地）

- **S3 全过（13ms）**：SA 语义层校验**纯函数** + 坏编排对抗集 —— 全部不需要 LLM/世界/时钟（07 §0）。
- **新增 `examples/sa_demo/sa_guard.hpp`**（demo 内、纯 std+nlohmann、零 cppchain 依赖）：
  `cppchain::sa` 命名空间的七个纯函数不变式 —— **I00 未知 agent / I01 域外或幻觉工具 /
  I02 核心只读（I1 静态面）/ I03 分诊目标∈已知域 / I04 里程碑∈词汇表 / I05 预算（≈V4）/
  I06 循环迹象（S4）/ I07 任务断言（≈V6）**；辅助：`canonical_json`（参数指纹）、`usage_from_trace`
  （轨迹自证用量，宿主不口报）。
- **V1–V8 取舍（显式落档）**：**V4→I05 · V6→I07**；I01/I03/I04/I06 = 组编排层新增不变式；
  **V1/V2/V3/V5/V7/V8 因 agent-group 形态无 DAG/waits/pivot → 标注 N/A，不硬凑**（等 Graph Runtime
  形态出现再补）。
- **demo 三形态**：① **单元自检**（oracle 直灌 12 条好/坏样本，坏样本必须被拒：幻觉工具 / 越域 /
  未知 agent / 核心写工具 / 不存在域 / 词汇表外里程碑 / 预算超限×2 / S4 循环 / 缺断言）；
  ② **集成过检**（S2 真实轨迹走完整验证链 —— 白名单=实际部署注册表防漂移；用量账轨迹自证：
  core 咨询 2≤3 · phone 7≤8 · msg 1≤4 · 重试 2≤2；里程碑 7 条全∈词汇表）；
  ③ **运行时对抗**（脚本桩弹幻觉工具 `samock_send_sms` → **ToolNode 既有失败关闭**回喂
  `unknown tool` error、咨询不中断、世界零污染）——**分工：验证器=审、ToolNode=挡**。
- 轨迹来源 = **录迹包装**：域注册表 handler 外包一层 recorder，域 agent 每把真实工具调用落 `trace`
  （验证器消费的是运行时事实，不是导演口报）。
- 下一步：**S4** —— SA 专用 UI 页（本轮已落，见下）。

### 第 4 轮（2026-11-03 · S2 落地）

- **S2 全过（10ms）**：假电话 FSM 进 samock + C7 简版组织闭环，24 项断言全绿。
- **samock 新增**：
  - **平台侧电话 FSM**：每次拨号 = 一个 Session，`phase = fold(剧本, dialed_at, now)` **纯派生、无后台线程**
    （可回放、逐字节确定）；剧本 = attempt1 振铃@3s/拒接@10s、attempt2 振铃@3s/接通@8s；
  - **里程碑事件**：dialing / ringing / rejected / answered / ended —— 相位跳变时**原子发一次**（幂等）、
    事件时间取精确时刻（非查询时刻）；`samock_milestones` 按 **seq 游标**读增量（= 观测归约）；
  - **`samock_barge_in`**：控制平面注入用户改口（仅 Talking 态有效——负路径留 F 系列故障注入），
    平台挂断并发 `ended{reason=user_redirected}`。
- **demo 新增 S2 导演编排**（全离线，脚本桩模型**弹 tool_calls**、Agent 循环+ToolNode 真跑工具，非导演代跑）：
  核心 2 次咨询（初始分诊 + 改道编排 ≤3）· 电话域域内拒接→重拨 attempt2→接通（核心不醒）·
  barge_in 直灌 · 消息域 send_message 收尾。**24 项断言**：世界终态（c1 ended / user_redirected /
  redirect_to_messaging）· 里程碑全序恰 7 条（dialing:1,ringing:1,rejected:1,dialing:2,ringing:2,answered:2,
  ended:2）· seq==7 · now_sec==27（零真实等待）· 核心模型咨询==2（拒接/振铃/重拨全程未打扰核心，
  C7「唤醒 7→3」方向兑现）· 电话域 9 轮 / 消息域 2 轮。
- 下一步：**S3** —— SA 语义层校验纯函数（V1–V8 子集）+ 坏 patch/坏模型对抗集（07 §5.2）。

### 第 3 轮（2026-11-03 · S1 demo 落地）

- 用户拍板 4 点：领域初集 = **A 电话域 + 消息域**；demo 要 **mock 电话**；摆放
  `examples/sa_demo/` + `tests/plugin_demo/samock.cpp`；虚拟钟 v1 用**工具推进**即可。
- **S1 落地并验证（11ms 全过）**：
  - `tests/plugin_demo/samock.cpp` → `libsamock.so`：世界模型（calls/messages/side_effects 只追加台账）+
    三诚实度（honest/lying/partial）+ 虚拟钟（工具推进、确定性从 0 起步）+ v0 能力（make_call/send_message）
    + world_state（断言唯一入口，永远说真话）+ world_reset（跨用例隔离）；纯 C descriptor、不链 cppchain。
  - `examples/sa_demo/main.cpp` 全离线（脚本桩模型，S1 不发起对话）：
    ① samock 装载走现有 **`tool::load_dynamic_tools`**（wfr 动态工具库同一加载器，fail-closed）；
    ② 世界空跑六场景（A 空基线 / B honest·真做+幂等 / C lying·说谎被抓 / D partial·半写 /
      E 虚拟钟 / F 重置），**断言只查世界不查返回值**；③ SA 组装配（核心 + 电话域 + 消息域三实例）+
      I1 检查（核心 agent 工具集全只读）。
  - `./run-demo.sh sa_demo` **验收结论：S1 全部通过**。
- **实现口径（与 §5.2 提案的两点差异，按实况落档）**：`add_library(samock SHARED ../tests/plugin_demo/samock.cpp)`
  建在 **examples/CMakeLists.txt**（与 `mcp_weather_list` 同先例：demo dlopen 的 so 随 examples 构建目录出，
  与 demo 二进制同目录）；装载不手写 dlopen，用现成 `tool::load_dynamic_tools`；samock 定位顺序 =
  env `CPPCHAIN_SAMOCK_DIR` → 编译期 `SA_SAMOCK_DIR`（examples 构建目录）。
- 下一步：**S2** —— 假电话 FSM（Dialing→Ringing→Connected→Ended|Failed）+ barge_in 进 samock，
  C7 简版组织闭环（核心→电话域→消息域，打电话→改口→转消息），先离线脚本桩、再决定真机。在线验证再收口。

### 第 2 轮（2026-11-03 · 拍板 cppchain 落地形态）

- **落点拍板**：基于 **cppchain** 做（非 DSH 宿主）；研究组 01–08 作语义基线，落点时映射到 cppchain 既有件。
- **组织形态拍板**：SA = **一组 agent**（核心 agent + 领域 agents），复用 cppchain 多智能体原语。
- **载体拍板**：SA 专属 **demo 目录** + **独立 `samock` so**（mock 代码全进 samock，参照 `sidefx_notify.so`
  先例：tests/plugin_demo 源 + `add_library(SHARED)` + env 目录加载）。
- 已产出：§六 落地方案骨架（demo / samock / 领域 agent 初集 / 落点映射），待逐条确认后开工。

### 第 1 轮（2026-11-03）

- 用户指认研究源：`docs/01-discuss/system-agent/`（01–07 + `_research/01–05`）+
  `system-agent-doubao/system-agent-desigin.md`，要求整理三件事：**① 核心架构 ② 测试框架 ③ Mock 外围**。
- 产出整理稿 [`system-agent/08`](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md)；
  §一 定义转正（上节）；§三 盘点改为指向真实研究源。
- **待续（08 §四 开放项）**：落点（DSH vs cppchain 平移）；`02 §10.2` Q1–Q12 与 `07 §11` Q-m1–Q-m5
  待拍板项合表收敛；是否以 **M0（patch 直灌 + 验证器纯函数，2~3 天零基建）** 做第一刀 demo。

### 第 0 轮（开题，2026-11-02）

- 用户提出开专题研究 SystemAgent，要求先把专题开起来、内容逐点讨论。
- 本文件 = 编号 64，problem 线开题骨架；README 已知问题清单 + 文件列表已登记。
- 下一步：第一轮讨论定「SystemAgent 指什么」（即 §一，已由第 1 轮完成）。

## 三、研究源盘点（第 1 轮已定位）

- **权威研究源**（SA 的定义与演进全部在此）：`docs/01-discuss/system-agent/` 01–07 + `_research/01–05`——
  02 架构草案为骨骼，01 业界调研 + 03–07 case 反证/推演为修正链；一手基线 `_research/05`（DSH 源码取证）。
- **规格型并行稿**：`docs/01-discuss/system-agent-doubao/system-agent-desigin.md`——无日期无作者，
  分层（五层）与边语义更清、时间停在"待深化问题"；**以 group 演进稿为准**（08 §0.3 差异对照，不合并只标注）。
- **整理稿**：[`system-agent/08`](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md)
  （核心架构 / 测试框架 / Mock 外围三合一，本专题当前研究主线）。
- **宿主与落点（开放项）**：全部设计以 **DSH（deepseek-harness）** 为宿主（`agent-loop` 是唯一含循环逻辑的
  包、不可 fork，SA 以插件寄居循环外）。若后续落 **cppchain**，需为 DSH 的
  `ctx.jobs` / `send(target×wakeup)` / `schedule` / `agent-loop-testkit` / `llm-mock-server` /
  `llm-replay` / `invariants` 找对应物或平移——这是待讨论的落点问题，不是已定事实。CPPChain 侧既有
  多智能体（`mch`/handoff）、project 自管理等仅作外围血缘，非 SA 本体。

## 四、专题流程约定（沿用项目基线）

1. **问题 + demo 驱动**（见 [discuss 01](../01-discuss/01-项目基调与构建原则.md)/README）：每个论点
   要有可运行验证，不写空头方案；真机模型按项目惯例（DeepSeek-flash 够用时不上大模型）。
2. 推进节奏：研究/盘点（本篇）→ 设计（arch）→ 落地（detail / problems 验收）→ 全仓测试 + 真机对照。
3. 数字与编号纪律：全仓串行测试全绿后才写「验收」结论；文档与报告数字一致性是硬要求。

## 五、落地方案骨架（cppchain · 提案，待逐条确认）

### 5.1 SA 的组成：核心 agent + 领域 agents（一组 agent）

```
                        ┌──────────────────────────────┐
   事件 / 用户 query ──► │   SA 核心 agent（编排者）        │
                        │  分诊 → 选域 → 编排拓扑 → fallback │
                        └───────┬──────────┬──────────┘
                     handoff / Send / 子 workflow / ask_human
              ┌───────────┴───┐         ┌───┴───────────┐
       ┌──────▼──────┐  ┌─────▼──────┐  ┌▼──────────────┐
       │ 领域 agent A │  │ 领域 agent B│  │ 领域 agent …   │
       │ 域内能力+FSM  │  │ 域内能力+FSM │  │               │
       └──────┬──────┘  └─────┬──────┘  └┬──────────────┘
              │  工具均指向 samock 世界   │
              └─────────► [libsamock.so 世界 / 假能力 / 事件源]
```

- **核心 agent**：不做域内执行，只决定"谁来做、怎么做、做不了怎么办"（对应研究稿
  「SA 只编排不执行」「分诊=编排的退化结论」）；用 cppchain 的 agent loop / handoff / 子 workflow 落地。
- **领域 agent**：各持一个领域的能力与（可选）状态机，执行域内工作；对应研究稿三类节点里的
  **AgentLoop 节点**（封闭 ReAct 循环，只上报终态与里程碑）。
- **事件入口**：query / 模拟事件流统一进核心 agent；C6 式漏斗若第一版不建，先用目录级最小兜底
  （事件直接进核心 agent，数量少时成本可控），漏斗留待第二版。

### 5.2 目录与构建（沿用仓库先例）

```
examples/sa_demo/                    # SA 专属 demo 目录（一个可执行目标）
  README.md                          # 索引行 + 运行方式
  main.cpp                           # 组装核心 agent + 领域 agents + samock 装载 + S2 导演 + S3 验证
  sa_guard.hpp                       # S3（M0）：SA 语义层校验纯函数（I00–I07，零 cppchain 依赖）
  scenarios/                         # 场景数据（C7 简版 / 对抗坏 patch…），先内联后抽数据
  # 登记到 examples/CMakeLists.txt 的 EXAMPLE_TARGETS（sa_demo → SA_EXAMPLE_TARGETS，链 cppchain_agent+tool）

tests/plugin_demo/samock.cpp         # 所有 SA 专属 mock 代码 → libsamock.so
examples/CMakeLists.txt              # add_library(samock SHARED ../tests/plugin_demo/samock.cpp)
                                     # 先例同 mcp_weather_list（demo dlopen 的 so 随 examples 构建出、
                                     #   与 demo 同目录）；纯 C descriptor（tool_plugin.hpp）、不链 cppchain；
                                     # add_dependencies(sa_demo samock)；
                                     # compile-def SA_SAMOCK_DIR = examples 构建目录

web/sa/index.html                    # SA 专用 UI 页（自包含单文件；run-webui.sh → http://localhost:8000/sa/）
web/sa/sample-tasklog.json           # 内置样例（提交版）；sa_tasklog.json = demo 运行产物（gitignore）
```

- demo 运行：`./run-demo.sh sa_demo`（装载走现有 **`tool::load_dynamic_tools`**，即 wfr 动态工具库同一加载器；
  samock 定位顺序 = env `CPPCHAIN_SAMOCK_DIR` → 编译期 `SA_SAMOCK_DIR`）。
- **隔离纪律**（07 R5）：CI/常规测试路径**不得触碰真模型**；demo 里 LLM 可走 .env 真机
  （延续 chat_live/phantasma 惯例）或走离线对博弈（scripted/adversarial），二者物理分开。

### 5.3 samock.so 第一版装什么（全对 mg 拟件，N 编号沿用研究稿）

| 件 | 研究稿 | samock v1 落地 | 状态 |
|---|---|---|---|
| 世界模型 | N2 | 内存 `World`（calls/messages/side_effects 只追加台账 + seq 单调序号）+ 三诚实度
  （honest/lying/partial）；`world_state` 序列化整个世界（**断言唯一入口，永远说真话**）；重置含钟/诚实度 | ✅ **S1 已装**（`samock_world_state`/`samock_world_reset`；断言逻辑在 demo 侧 C++，见 07「查世界不查返回值」） |
| 假电话 FSM | N3 | 每次拨号=Session，`phase=fold(剧本,dialed_at,now)` 纯派生；剧本=attempt1 拒接/attempt2 接通；
  里程碑（dialing/ringing/rejected/answered/ended）seq 游标增量读；`samock_barge_in` 控制平面（仅 Talking 有效） | ✅ **S2 已装**（`samock_make_call`+`samock_milestones`+`samock_barge_in`；消息侧仍 v0） |
| 事件源 | — | `event_storm`（C6 洪峰注入）+ 源侧聚合（G-l 简化版） | ⏳ 待 S2+ |
| 虚拟时钟 | N1 | **工具推进**的模拟钟（`samock_clock_forward`，确定性从 0 起步、累加式）；全套 quiescence
  调度器留待 M2 需要时再上 | ✅ **S1 已装** |
| 场景 DSL | N4 | S1 场景内联在 demo（C++ 数据直灌工具），抽 YAML 作为 M1 升级项 | 🔶 S1 内联（六场景脚本），YAML 化待后续 |

> 领域 agent 不是 mock，是真实 demo 代码（在 `examples/sa_demo/` 内）；**mock 的一切都进 samock.so**。

### 5.4 DSH → cppchain 落点映射（待落地时逐个实核）

| DSH 现成件（研究稿 _research/05） | cppchain 对应 | 处置 |
|---|---|---|
| `ctx.jobs`（句柄+增量+取消） | 待盘点：tool/workflow 长任务形态（command_run / 并行波 Send） | 实核后补或列为缺口 |
| `send(target×wakeup)`（事件→对话准入） | 待盘点：cppchain 事件/唤醒等价物 | 可能需 SA 宿主循环承接 |
| `schedule`（持久化定时+冷恢复） | wfr cron（触发接入，problems/56）浅近似 | 持久化续体=研究 G2，列缺口 |
| `agent-loop-testkit` / `llm-mock-server` / `llm-replay` / `invariants` | cppchain 测试侧离线 mock model / 断言基建 | 实核后确认可平替项 |
| agent 循环（不可 fork） | cppchain `agent/agent.hpp` 即库里循环，天然可组合 | 直接用 |

**真正要新建的（cppchain 版结论，对应研究稿"只新建 Graph Runtime"）**：SA 语义层
——事件漏斗（观测归约）、L1 任务表 + L2 瞬时装配、咨询 gate（LLM 只出现声明的接缝）、
4 级升级 + Reaper 善后、patch 记账与验证映射到 workflow IR 校验。

### 5.5 里程碑进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **S1** | `examples/sa_demo` 骨架：核心 agent + 电话域/消息域 agents + samock 装载 + 世界空跑六场景 + 组装配/I1 | ✅ **2026-11-03 全过**（`run-demo.sh sa_demo`，11ms） |
| **S2** | samock **假电话 FSM**（Session 剧本 fold + 里程碑 seq 游标 + barge_in 控制平面）→ demo 跑通 **C7 简版**（打电话→改口→转消息，离线脚本桩组织闭环，24 断言） | ✅ **2026-11-03 全过**（10ms） |
| **S3** | M0 化：SA 语义层校验纯函数（V1–V8 子集 → I00–I07）+ 坏编排对抗集（07 §5.2）+ 运行时挡幻觉工具 | ✅ **2026-11-03 全过**（13ms） |
| **S4** | **SA 专用 UI 页**（`web/sa/index.html`）：左任务列表 / 右详情 50% 可拖动（上 DAG 节点图下消息列表）；数据契约 `sa-tasklog/v1`；t1 C7 简版 + t2 消息直路两任务 | ✅ **2026-11-03 全过**（17ms） |
| **S5** | **浏览器触发运行**：`sa_cgi` CGI 端点（`/cgi-bin/sa_cgi?scenario=all` —— spawn sa_demo + run_meta 维护，v1 仅 all）+ 页面「▶ 运行 demo」按钮 | ✅ **2026-11-03 全过**（11ms 回执，http --cgi 全通路） |
| **S6** | **WebUI E2E**：`run-sa-e2e.sh` 三阶段 —— A 静态 HTTP（页面骨架+sample 契约）/ B CGI 运行整路（回执·磁盘三产物·run.log verdict·确定性数值·二次递增·v1 拒答）/ C headless Chrome（渲染 + 真实点击「运行 demo」→「运行完成」）；11/11 | ✅ **2026-11-03 全过** |
| **S6b** | **右上面板「源码」标签页（图源码）**：显示**当前任务 DAG 的定义**（节点×边清单 ⇄ 原始 JSON，纯前端）；**Round 10 修正**——撤去误做的「读仓库 C++ 文件」与 `sa_cgi?action=src` 端点；E2E 11/11（C 阶段断言图源码渲染） | ✅ **2026-11-03 全过** |
| **S6c** | **运行可视化（真慢跑 + 实时进度）**：`sa_demo --progress/--pace-ms` 边跑边写 JSONL（run/task/dag/node/msg），`sa_cgi?pace=` 控制节奏；页面轮询后节点逐个点亮、消息逐条出现；E2E **12/12**（B 阶段进度事件断言）；tasklog 与 sample 逐字节等价 | ✅ **2026-11-03 全过** |
| **S7** | **一句话 → DAG → 运行**：真机 LLM Planner（`--query` / CGI `?planner=llm&query=`）产出受限 IR → `sa_plan.hpp` P00–P29 静态校验（fail-closed，回喂重试≤3）→ 通用解释器按拓扑序调度电话/消息域 + 世界断言；页面输入框 + 实时 DAG；`--plan-json` 离线可测；`sa_plan_tests` 12 条 + E2E **14/14** + 全仓 **1092/1092** | ✅ **2026-11-03 全过** |
| **S8** | **任务历史 + 删除 + 下载 + 更多示例**：`sa_tasklog.json` 即历史库（稳定 id 覆盖 / 规划任务追加 / 最近优先 / 上限 60 / 原子写）；`sa_cgi?action=delete&id=` 持久删除；`flock` 单运行锁防并发覆盖；页面任务卡 `×`、下载当前/历史 JSON、3 个示例 chip；E2E **16/16** | ✅ **2026-11-03 全过** |
| S7+ | 事件漏斗 / L1 任务表 / Reaper 善后 / 落地验收（C1–C12 抽角 C2/C3/C4/C6/C7/C8）；SA-CGI v2 单场景触发 | ⏳ 下一步 |

**落地前已拍板**：领域初集 = **电话域 + 消息域**（A 组）；demo 需要 **mock 电话**；
摆放 = `examples/sa_demo/` + `tests/plugin_demo/samock.cpp`；虚拟钟 v1 = **工具推进**的模拟钟（够用）。

## 六、关联

- 研究主线：[`docs/01-discuss/system-agent/`](../01-discuss/system-agent/README.md)（01–07 + `_research`）+
  整理稿 [08](../01-discuss/system-agent/08-SA核心架构测试框架与Mock外围整理.md) +
  doubao [设计档](../01-discuss/system-agent-doubao/system-agent-desigin.md)。
- 编号承接：problems 最大 63（[长上下文蜀山案例停置收口记录](63-长上下文蜀山案例停置收口记录.md)）；
  本篇=64，后续子篇顺延。
- 相邻面：`高可信/`（做得对不对）+ `主动智能/`（什么时候开始）= SA 相邻（SA 管"开始之后以什么形态运行"）。
- 内存关联：`cppchain-rsi-plan`（Agent 自进化）、`cppchain-personal-agent-plan`。